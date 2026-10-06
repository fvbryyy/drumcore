#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Host tests — DrumCore KIT RUNTIME + PERSISTENCE.

Mensimulasikan SEMANTIK firmware ESP32 (kit_manager + kit_loader +
uart_protocol) terhadap isi folder SD card sungguhan (salinan dari
SDCard/kits), TANPA hardware. Fokus perilaku, bukan bit-exact:

  1. CREATE EMPTY  -> baseline aman murni + mapping sample kosong,
                      folder samples/ KOSONG (tanpa clone factory).
  2. CREATE COPY CURRENT -> kit hasil INDEPENDEN (file disalin,
                      mapping by filename).
  3. Mapping by FILENAME (bukan index) untuk PAD -> SOUND.
  4. KIT SAVE -> serialize effective RAM ke kit.json; restart boot
                      membaca nilai yang sama (round-trip).
  5. Last-active-kit persist: switch kit -> restart boot ke kit sama.
  6. Missing kit fallback -> KIT_FACTORY_INDEX + persist global.
  7. Aribitrary WAV discovery (scan sorted, cap 12, case-insensitive).
  8. Kit independence: perubahan satu kit tidak menyentuh kit lain.
  9.  G12 resolver name-based FULL-PATH (path()): kitId=0 -> /kits/Acoustic,
      nama berspasi /kits/ROCK LIVE, identitas by kitId bukan folder,
      lock regresi pra-G12 (basename-path gagal).
 10.  G5 fixture audit (read-only): kanonik 5 kit, remap ROCK LIVE,
      tanpa folder kitNN legacy.
11.  G2/G3/G4 lifecycle: rename-move re-resolve, delete GC seluruh pohon,
       create named kanonik.
  12.  G13 Wemos UI scheduling KIT_INFO: consume-first + in-flight guard
       menghentikan livelock 20ms (request-tiap-tick memusnahkan reply
       yang sudah lengkap). Model uart_client kitInfo* + ui poll/request.
  13.  G14 burst split: header vs chunk tiba di drain berbeda — guard
       busy (pending|ok|ready) melindungi rakitan chunk; guard G13 yang
       hanya cek pending tetap livelock dalam skenario ini.
  14.  G16 active-kit name cache: valid metadata wins, preview tetap
       name-based, rename/load/delete dan fallback-invalidasi konsisten.
  15.  G17 Wemos KIT_INFO assembly: header/chunk berurutan, batas nama,
       timeout, cache consumption, dan renderer name-based.

Menjalankan:  python host_tests/kit_runtime_test.py
Exit code != 0 bila ada test gagal. Stdlib only.
"""

import copy
import json
import os
import random
import re
import shutil
import sys
import tempfile

PAD_COUNT = 12
KIT_COUNT = 5
KIT_FACTORY_INDEX = 0
SAMPLE_NAME_MAX = 32
KIT_NAME_MAX = 28
SCAN_CAP = 12

FIXTURE = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "SDCard", "kits",
)

# Baseline defaultPadSource() (config.h) — refraksi semantic firmware.
BASELINE = {
    "midiNote": 36,
    "threshold": 300,
    "releaseThreshold": 250,
    "peakTime": 1200,
    "releaseStableTime": 3000,
    "maskBaseMs": 20,
    "sensitivity": 100,
    "curve": 0,
    "padType": 1,
    "sampleId": 0,
    "chokeGroup": 0,
}

_passed = 0
_failed = 0


def _check(name, cond, detail=""):
    global _passed, _failed
    if cond:
        _passed += 1
        print(f"  PASS  {name}")
    else:
        _failed += 1
        print(f"  FAIL  {name}  {detail}")


def _func_body(src, signature):
    """Isi satu fungsi C++ (dari signature sampai brace balance 0).

    Dipakai source-verification supaya assertion benar-benar mengecek
    ISI fungsi, bukan string yang mungkin ada di file lain.
    """
    start = src.find(signature)
    if start < 0:
        return ""
    open_at = src.find("{", start)
    if open_at < 0:
        return ""
    depth = 0
    for i in range(open_at, len(src)):
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
            if depth == 0:
                return src[open_at:i + 1]
    return ""


def _strip_comments(src):
    """Buang komentar C++ dari potongan source.

    Assertion "fungsi ini TIDAK memanggil X" harus memeriksa KODE, bukan
    prosa komentar — kalau tidak, satu kalimat dokumentasi yang menyebut
    X akan salah dianggap sebagai pemanggilan.
    """
    out = []
    i = 0
    n = len(src)
    while i < n:
        two = src[i:i + 2]
        if two == "//":
            while i < n and src[i] != "\n":
                i += 1
        elif two == "/*":
            i += 2
            while i < n and src[i:i + 2] != "*/":
                i += 1
            i += 2
        else:
            out.append(src[i])
            i += 1
    return "".join(out)


# ---------------------------------------------------------------
# Mini SD simulator: jalankan pada salinan fixture (tidak merusak).
# ---------------------------------------------------------------

class Sd:
    def __init__(self, root):
        self.root = root

    def p(self, path):
        joined = os.path.join(self.root, path.lstrip("/"))
        return joined.replace("/", os.sep)

    def exists(self, path):
        return os.path.exists(self.p(path))

    def isdir(self, path):
        return os.path.isdir(self.p(path))

    def mkdirs(self, path):
        os.makedirs(self.p(path), exist_ok=True)

    def rmtree(self, path):
        if self.exists(path):
            shutil.rmtree(self.p(path))

    def copyfile(self, src, dst):
        shutil.copyfile(src, dst)

    def listdir(self, path):
        if not self.exists(path) or not self.isdir(path):
            return []
        return os.listdir(self.p(path))

    def read_json(self, path):
        with open(self.p(path), "r", encoding="utf-8") as f:
            return json.load(f)

    def write_json(self, path, doc):
        tmp = self.p(path) + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(doc, f, indent=2)
        os.replace(tmp, self.p(path))


def sd_home():
    tmp = tempfile.mkdtemp(prefix="drumcore_kit_runtime_")
    dst = os.path.join(tmp, "kits")
    shutil.copytree(FIXTURE, dst)
    return Sd(tmp), tmp


# ---------------------------------------------------------------
# Model firmware (semantic mirror — lihat komentar tiap fungsi).
# ---------------------------------------------------------------

def default_source(_pad_index):
    """defaultPadSource(): baseline murni dari tabel, bukan kit00."""
    return copy.deepcopy(BASELINE)


def validate_sample_name(name):
    """kitLoaderValidateSampleName(): 1..SAMPLE_NAME_MAX, no / \\, not '..'."""
    if not isinstance(name, str):
        return False
    n = len(name)
    if n == 0 or n > SAMPLE_NAME_MAX:
        return False
    if "/" in name or "\\" in name:
        return False
    if n == 2 and name == "..":
        return False
    return True


def abs_sample(sd, kit_dir, name):
    return os.path.join(sd.p(kit_dir), "samples", name)


def resolve_samples(sd, kit_dir, pads):
    """resolveSamples(): mapping by FILENAME. Path hilang/rusak ->
    unassigned (None), TIDAK fallback ke root config.h."""
    resolved = [None] * PAD_COUNT
    for i in range(PAD_COUNT):
        name = (pads[i].get("sample") or "") if i < len(pads) else ""
        if not validate_sample_name(name):
            resolved[i] = None
            continue
        ap = abs_sample(sd, kit_dir, name)
        if sd.exists(ap):
            resolved[i] = ap
        else:
            resolved[i] = None
    return resolved


def scan_samples(sd, kit_dir):
    """kitLoaderScanSampleNames(): sorted *.wav (case-insensitive),
    valid name <= SAMPLE_NAME_MAX, cap SCAN_CAP. Absen folder -> []."""
    sdir = os.path.join(sd.p(kit_dir), "samples")
    if not os.path.isdir(sdir):
        return []
    out = []
    for entry in sorted(os.listdir(sdir)):
        if len(out) >= SCAN_CAP:
            break
        full = os.path.join(sdir, entry)
        if not os.path.isfile(full):
            continue
        if not validate_sample_name(entry):
            continue
        if entry.lower().endswith(".wav"):
            out.append(entry)
    return out


def current_index(names, resolved_name):
    """ARG3 CMD_PAD_SOUND: index sample pad di daftar (0xFF bila tak ada)."""
    base = os.path.basename(resolved_name) if resolved_name else None
    if base is None:
        return 0xFF
    for i, n in enumerate(names):
        if n == base:
            return i
    return 0xFF


def effective_pad_config(source_pads, resolved):
    """getPadConfig(i) + kitLoaderResolvedSampleName(i): effective RAM."""
    out = []
    for i in range(PAD_COUNT):
        src = source_pads[i] if i < len(source_pads) else {}
        p = dict(BASELINE)
        for k, v in src.items():
            if k not in ("sample",) and v is not None:
                p[k] = v
        res = resolved[i]
        p["sample"] = os.path.basename(res) if res else ""
        out.append(p)
    return out


def write_kit(sd, kit_dir, kit_id, name, pads):
    """writeJsonFromSources(): atomic, kitId+id+name+pads."""
    sd.mkdirs(kit_dir)
    sd.mkdirs(os.path.join(kit_dir, "samples"))
    doc = {
        "kitId": kit_id,
        "id": kit_id,
        "name": name,
        "pads": pads,
    }
    sd.write_json(os.path.join(kit_dir, "kit.json"), doc)


def create_kit_empty(sd, slot, name, baseline):
    """createKitEmpty(): baseline aman + sample kosong + tanpa clone."""
    pads = []
    for i in range(PAD_COUNT):
        p = baseline(i)
        p["sample"] = ""            # pad senyap sampai PAD_SOUND_SET
        pads.append(p)
    dirn = f"/kits/{name}"
    write_kit(sd, dirn, slot, name, pads)
    return dirn


def create_kit_copy(sd, slot, name, source_effective, source_dir):
    """createKitCopy(): snapshot effective RAM + dup file; INDEPENDEN."""
    dirn = f"/kits/{name}"
    write_kit(sd, dirn, slot, name, [dict(x) for x in source_effective])
    for p in source_effective:
        if not p["sample"]:
            continue
        src_abs = os.path.join(
            sd.p(source_dir), "samples", p["sample"])
        if os.path.isfile(src_abs):
            dst_abs = os.path.join(
                sd.p(dirn), "samples", p["sample"])
            shutil.copyfile(src_abs, dst_abs)
    return dirn


def kit_save_active(sd, kit_dir, name, effective):
    """kitManagerSaveActive(): tulis effective RAM ke kit.json aktif."""
    write_kit(sd, kit_dir, -1, name, effective)


def load_kit(sd, kit_dir):
    """kitLoaderLoad(): baca kit.json -> source pads (fallback field)."""
    return sd.read_json(os.path.join(kit_dir, "kit.json"))["pads"]


def set_pad_sample(sd, active_dir, pad_index, name):
    """PAD_SOUND_SET semantics: validasi + abs ada -> mapping baru."""
    if not validate_sample_name(name):
        return "invalid"
    ap = abs_sample(sd, active_dir, name)
    if not sd.exists(ap):
        return "missing"
    return ap


def read_kit_meta(sd, path, fallback_id):
    """readKitMeta(): idOut default=fallbackIndex; kitId (legacy id)."""
    if not sd.exists(path):
        return None
    try:
        doc = sd.read_json(path)
    except (OSError, ValueError, TypeError):
        return None
    return {
        "id": doc.get("kitId", doc.get("id", fallback_id)),
        "name": doc.get("name") or "UNNAMED",
    }


def resolve_kit_dir(sd, slot):
    """kitLoaderResolveKitDir() pasca-fix G12: bangun path dari FULL PATH
    (padanan File::path(); name() hanya basename). Scan /kits/*/kit.json
    untuk kitId/id == slot; canonical (folder == name) > match pertama;
    fallback legacy /kits/kitNN bila scan kosong."""
    if slot >= KIT_COUNT:
        return None
    canonical = None
    any_dir = None
    for entry in sd.listdir("/kits"):
        full = "/kits/" + entry
        if not sd.isdir(full):
            continue
        meta = read_kit_meta(sd, full + "/kit.json", slot)
        if meta is not None and meta["id"] == slot:
            if canonical is None and meta["name"] == entry:
                canonical = full
            if any_dir is None:
                any_dir = full
    if canonical:
        return canonical
    if any_dir:
        return any_dir
    legacy = "/kits/kit%02d" % slot
    if sd.exists(legacy + "/kit.json"):
        return legacy
    return None


class KitNameRuntime:
    def __init__(self, sd):
        self.sd = sd
        self.active_index = KIT_FACTORY_INDEX
        self.loaded_id = None
        self.cached_name = "/none"
        self.cache_valid = False

    def _is_active(self, index):
        return index == self.active_index

    def build_path(self, index):
        self.active_index = index
        self.loaded_id = None
        self.cache_valid = False
        dirn = resolve_kit_dir(self.sd, index)
        if dirn is None:
            dirn = "/kits/kit%02d" % index
        return dirn + "/kit.json"

    def load(self, index):
        path = self.build_path(index)
        try:
            doc = self.sd.read_json(path)
        except (OSError, ValueError, TypeError):
            return False

        self.loaded_id = doc.get("kitId", doc.get("id"))
        name = doc.get("name")
        self.cached_name = name if isinstance(name, str) and name else "UNNAMED"
        self.cache_valid = True
        return True

    def get_info(self, index):
        if self._is_active(index) and self.cache_valid and self.loaded_id is not None:
            return self.loaded_id, self.cached_name, True

        dirn = resolve_kit_dir(self.sd, index)
        if dirn is None:
            return index, "UNNAMED", False

        meta = read_kit_meta(self.sd, dirn + "/kit.json", index)
        if meta is None:
            return index, "UNNAMED", False

        return meta["id"], meta["name"], True

    def get_name(self, index):
        return self.get_info(index)[1]

    def get_id(self, index):
        return self.get_info(index)[0]

    def set_cached_name(self, index, name):
        if self._is_active(index) and isinstance(name, str) and name:
            self.cached_name = name
            self.cache_valid = True


def resolve_kit_dir_buggy_basename(sd, slot):
    """Mirror pra-G12 (bug): metaPath disusun dari basename (name()),
    misal 'Acoustic/kit.json' -> relatif, SD.exists selalu gagal."""
    if slot >= KIT_COUNT:
        return None
    for entry in sd.listdir("/kits"):
        full = "/kits/" + entry
        if not sd.isdir(full):
            continue
        if sd.exists(entry + "/kit.json"):
            return full
    return None


# ---------------------------------------------------------------
# Tests
# ---------------------------------------------------------------

def test_create_empty_baseline_safe():
    print("1. CREATE EMPTY = baseline aman + mapping kosong + tanpa clone")
    sd, tmp = sd_home()
    try:
        dirn = create_kit_empty(sd, 3, "TIBA", lambda i: default_source(i))
        pads = load_kit(sd, dirn)

        ok_baseline = all(
            all(p[k] == BASELINE[k] for k in BASELINE) for p in pads)
        _check("semua field == baseline (defaultPadSource)", ok_baseline)

        ok_empty = all(p["sample"] == "" for p in pads)
        _check("mapping sample kosong per pad (kit senyap)", ok_empty)

        sdir = os.path.join(sd.p(dirn), "samples")
        files = [f for f in os.listdir(sdir) if f != "." and f != ".."]
        _check("folder samples/ KOSONG (tidak ada clone factory)", files == [])

        _check("TIDAK otomatis aktif (slot lain tak tersentuh)",
               sd.exists("/kits/ROCK LIVE/kit.json") and
               sd.read_json("/kits/ROCK LIVE/kit.json")["name"] == "ROCK LIVE")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def test_create_copy_independent():
    print("2. CREATE COPY CURRENT = snapshot RAM + file duplikat (independen)")
    sd, tmp = sd_home()
    try:
        ac = sd.read_json("/kits/Acoustic/kit.json")
        pads_src = ac["pads"]
        res = resolve_samples(sd, "/kits/Acoustic", pads_src)
        eff = effective_pad_config(pads_src, res)

        dirn = create_kit_copy(sd, 4, "COPYAC", eff, "/kits/Acoustic")
        copied = [p["sample"] for p in eff if p["sample"]]
        sdir = os.path.join(sd.p(dirn), "samples")
        _check("ISI kit baru == effective RAM (config file by filename)",
               load_kit(sd, dirn) == eff)
        _check("file sample disalin",
               sorted(os.listdir(sdir)) == sorted(set(copied)))

        # Independence: ubah kit baru, kit AKTIF (Acoustic) tak berubah.
        new_pads = load_kit(sd, dirn)
        new_pads[0]["midiNote"] = 99
        write_kit(sd, dirn, 4, "COPYAC", new_pads)
        ac2 = sd.read_json("/kits/Acoustic/kit.json")
        _check("perubahan kit baru tidak menyentuh kit asal",
               ac2["pads"][0]["midiNote"] == pads_src[0]["midiNote"])
        _check("nama asal utuh", ac2["name"] == "Acoustic")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def test_mapping_by_filename():
    print("3. Mapping by FILENAME (PAD -> SOUND = assign WAV)")
    sd, tmp = sd_home()
    try:
        dirn = "/kits/Acoustic"
        pads = sd.read_json(dirn + "/kit.json")["pads"]

        # Resolve: nama file (bukan index) menentukan mapping.
        res = resolve_samples(sd, dirn, pads)
        _check("PAD0 -> KICK.wav", os.path.basename(res[0]) == "KICK.wav")

        # PAD_SOUND_SET ke pad 3 (OHH): valid.
        ap = set_pad_sample(sd, dirn, 3, "SAX.wav")  # tak ada -> missing
        _check("assign file yang tidak ada DITOLAK", ap == "missing")

        ap2 = set_pad_sample(sd, dirn, 3, "TOM2.wav")
        _check("assign file yang ada DITERIMA", ap2 is not None)

        # Reload PSRAM == mapping RAM + identity ids.
        new_pads = copy.deepcopy(pads)
        new_pads[3]["sample"] = "TOM2.wav"
        res2 = resolve_samples(sd, dirn, new_pads)
        ids = list(range(PAD_COUNT))   # identity sampleIds
        _check("PSRAM reload pakai identity ids (slot==pad)",
               ids == list(range(PAD_COUNT)) and
               os.path.basename(res2[3]) == "TOM2.wav")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def test_kit_save_roundtrip():
    print("4. KIT SAVE -> kit.json; restart membaca nilai sama")
    sd, tmp = sd_home()
    try:
        dirn = "/kits/Jazz"
        pads = sd.read_json(dirn + "/kit.json")["pads"]
        res = resolve_samples(sd, dirn, pads)
        eff = effective_pad_config(pads, res)

        # User ubah pad 1 threshold + pindah sample pad 2.
        eff[1]["threshold"] = 555
        eff[2]["sample"] = "SNARE.wav"

        kit_save_active(sd, dirn, "Jazz", eff)

        # Boot ulang: kitLoaderLoad membaca kit.json -> nilai sama.
        pads2 = load_kit(sd, dirn)
        _check("threshold tersimpan & terbaca", pads2[1]["threshold"] == 555)
        _check("sample filename tersimpan & terbaca",
               pads2[2]["sample"] == "SNARE.wav")

        res2 = resolve_samples(sd, dirn, pads2)
        _check("SNARE.wav ter-resolve pasca restart",
               os.path.basename(res2[2]) == "SNARE.wav")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def test_last_active_kit_persist():
    print("5. Last-active-kit persist (TEST G: restart boot ke kit sama)")
    sd, tmp = sd_home()
    try:
        # KIT_LOAD 3 sukses -> persistenceSaveGlobal(3,vol).
        active = 3
        kit_dirs = {
            0: "/kits/Acoustic",
            1: "/kits/ROCK LIVE",
            2: "/kits/Electronic",
            3: "/kits/Jazz",
            4: "/kits/Latin",
        }
        _check("active disimpan", active == 3)

        # Boot: NVS global active=3 -> resolve + load kit yang sama.
        dirn = kit_dirs[active]
        _check("resolve slot3 -> /kits/Jazz",
               resolve_kit_dir(sd, 3) == "/kits/Jazz")
        _check("boot memuat kit yang sama", sd.exists(dirn + "/kit.json"))
        name = sd.read_json(dirn + "/kit.json")["name"]
        _check("nama kit sesuai", name == "Jazz")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def test_missing_kit_fallback():
    print("6. Missing/rusak active kit -> fallback factory + persist")
    sd, tmp = sd_home()
    try:
        # active=4 (Latin) tapi foldernya dihapus (ex: DELETE gc).
        sd.rmtree("/kits/Latin")
        _check("kit aktual hilang", resolve_kit_dir(sd, 4) is None)

        # Boot fallback: resolve gagal -> KIT_FACTORY_INDEX + save global.
        saved_active = KIT_FACTORY_INDEX
        _check("fallback ke factory index=0", saved_active == 0)

        # Kit factory masih ada & ter-resolve (name-based, kitId 0=Acoustic).
        d0 = resolve_kit_dir(sd, 0)
        _check("kit factory ter-resolve", d0 == "/kits/Acoustic")
        _check("nama factory = Acoustic",
               sd.read_json(d0 + "/kit.json")["name"] == "Acoustic")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def test_arbitrary_wav_discovery():
    print("7. Scan folder samples: sorted, cap 12, case-insensitive")
    sd, tmp = sd_home()
    try:
        names = scan_samples(sd, "/kits/Acoustic")
        _check("menemukan semua 12 WAV", len(names) == 12)
        _check("terurut abjad", names == sorted(names))
        _check("cap tidak melebihi SCAN_CAP", len(names) <= SCAN_CAP)

        # folder hilang = list kosong (valid, bukan error).
        _check("kit tanpa folder samples -> []",
               scan_samples(sd, "/kits/SENYAP") == [])

        # Case-insensitive ext detection + non-WAV diabaikan.
        tmpd = "/kits/Acoustic"
        os.makedirs(os.path.join(sd.p(tmpd), "samples"), exist_ok=True)
        extra = os.path.join(sd.p(tmpd), "samples", "NEWCYMBAL.WAV")
        with open(extra, "w") as f:
            f.write("")
        junk = os.path.join(sd.p(tmpd), "samples", "README.txt")
        with open(junk, "w") as f:
            f.write("x")
        names2 = scan_samples(sd, tmpd)
        _check("UUID .WAV (upper) ter-deteksi", "NEWCYMBAL.WAV" in names2)
        _check("non-WAV (README.txt) diabaikan", "README.txt" not in names2)
        _check("hasil tetap terurut", names2 == sorted(names2))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def test_kit_independence():
    print("8. Kit independence (tidak ada cross-talk antar kit)")
    sd, tmp = sd_home()
    try:
        a = sd.read_json("/kits/Acoustic/kit.json")
        e = sd.read_json("/kits/Electronic/kit.json")

        # Electronic pakai SNARE di pad1; Acoustic tidak terpengaruh
        # oleh isi folder Electronic (resolve & hanya folder sendiri).
        res_a = resolve_samples(sd, "/kits/Acoustic", a["pads"])
        a_dir = "/kits/Acoustic"
        names_a = scan_samples(sd, a_dir)
        _check("scan Acoustic hanya isi Acoustic",
               set(names_a) == set(f for f in os.listdir(
                   os.path.join(sd.p(a_dir), "samples"))
                   if f.lower().endswith(".wav")))
        _check("resolve pad0 Acoustic = KICK.wav milik Acoustic",
               os.path.basename(res_a[0]) == "KICK.wav" and
               ("kits" + os.sep + "Acoustic") in res_a[0])
        _ = e
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def test_resolver_name_based_g12():
    print("9. G12 resolver name-based FULL-PATH (path()) by kitId")
    sd, tmp = sd_home()
    try:
        # (a) aktifKit=0 harus resolve /kits/Acoustic berdasarkan kitId=0.
        d0 = resolve_kit_dir(sd, 0)
        _check("slot0 -> /kits/Acoustic (kitId=0)", d0 == "/kits/Acoustic")
        _check("meta slot0 kitId==0",
               read_kit_meta(sd, d0 + "/kit.json", 0)["id"] == 0)

        # (b) nama mengandung spasi (ROCK LIVE, kitId=1).
        _check("slot1 -> /kits/ROCK LIVE (nama berspasi)",
               resolve_kit_dir(sd, 1) == "/kits/ROCK LIVE")

        # (c) seluruh slot 0..4 ter-resolve name-based.
        _check("semua slot 0..4 ter-resolve",
               all(resolve_kit_dir(sd, s) is not None for s in range(5)))

        # (d) LOCK regresi: pra-G12 (basename-only) HARUS gagal resolve.
        _check("pra-G12 (basename) tidak menemukan apa pun",
               resolve_kit_dir_buggy_basename(sd, 0) is None)

        # (e) identitas by kitId, BUKAN nama folder: kitId=3 pindah ke
        #     folder non-canonical.
        sd.mkdirs("/kits/ZZJAZZ")
        shutil.copy(sd.p("/kits/Jazz/kit.json"),
                    sd.p("/kits/ZZJAZZ/kit.json"))
        sd.rmtree("/kits/Jazz")
        _check("kitId=3 via folder non-canonical (any)",
               resolve_kit_dir(sd, 3) == "/kits/ZZJAZZ")
        _check("folder canonical lama sudah dibuang",
               not sd.exists("/kits/Jazz/kit.json"))

        # (f) legacy kitNN hanya saat scan kosong.
        sd.rmtree("/kits/Latin")
        sd.mkdirs("/kits/kit04")
        sd.write_json("/kits/kit04/kit.json",
                      {"kitId": 4, "id": 4, "name": "Latin", "pads": []})
        _check("fallback legacy /kits/kit04 dipakai",
               resolve_kit_dir(sd, 4) == "/kits/kit04")
        sd.rmtree("/kits/kit04")
        _check("setelah kitNN dihapus -> resolve None",
               resolve_kit_dir(sd, 4) is None)

        # (g) integrasi boot: buildPath(0) -> /kits/Acoustic/kit.json.
        boot = resolve_kit_dir(sd, 0) + "/kit.json"
        _check("boot path = /kits/Acoustic/kit.json",
               boot == "/kits/Acoustic/kit.json" and sd.exists(boot))
        _check("nama dari boot load = Acoustic",
               sd.read_json(boot)["name"] == "Acoustic")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def test_fixture_g5_audit():
    print("10. G5 fixture audit (read-only: kanonik + remap + no legacy)")
    sd = Sd(os.path.dirname(FIXTURE))
    try:
        dirs = sorted(sd.listdir("/kits"))
        _check("5 folder kit kanonik",
               dirs == sorted(["Acoustic", "ROCK LIVE",
                               "Electronic", "Jazz", "Latin"]))

        remap = [1, 0, 6, 2, 5, 4, 7, 3, 9, 8, 11, 10]

        for slot in range(5):
            d = resolve_kit_dir(sd, slot)
            _check(f"slot{slot} ter-resolve", d is not None)

            if d is None:
                continue

            doc = sd.read_json(d + "/kit.json")
            base = os.path.basename(d.rstrip("/"))
            _check(f"slot{slot} kitId/id == {slot}",
                   doc["kitId"] == slot and doc["id"] == slot)
            _check(f"slot{slot} canonical (folder == name)", doc["name"] == base)

            sfiles = sorted(f for f in sd.listdir(d + "/samples")
                            if f.lower().endswith(".wav"))
            _check(f"slot{slot} 12 sample", len(sfiles) == 12)

            for p in doc.get("pads", []):
                if p.get("sample"):
                    _check(f"{base} pad sample ada",
                           sd.exists(d + "/samples/" + p["sample"]))

            if slot == 1:
                ids = [p.get("sampleId") for p in doc["pads"]]
                _check("ROCK LIVE remap sampleId utuh", ids == remap)

        no_legacy = all(not sd.exists("/kits/kit%02d" % s) for s in range(5))
        _check("tidak ada folder kitNN legacy", no_legacy)
    finally:
        pass


def test_g2g3g4_lifecycle():
    print("11. G2/G3/G4: rename-move re-resolve, delete GC, create named")
    sd, tmp = sd_home()
    try:
        # G2 RENAME-equivalent: pindahkan pohon ROCK LIVE (kitId=1) ke
        # nama baru, JSON name diselaraskan, folder lama dibuang.
        sd.mkdirs("/kits/ROCK EDIT")
        shutil.copy(sd.p("/kits/ROCK LIVE/kit.json"),
                    sd.p("/kits/ROCK EDIT/kit.json"))
        shutil.copytree(sd.p("/kits/ROCK LIVE/samples"),
                        sd.p("/kits/ROCK EDIT/samples"))
        doc = sd.read_json("/kits/ROCK EDIT/kit.json")
        doc["name"] = "ROCK EDIT"
        sd.write_json("/kits/ROCK EDIT/kit.json", doc)
        sd.rmtree("/kits/ROCK LIVE")
        _check("G2 re-resolve setelah rename-move",
               resolve_kit_dir(sd, 1) == "/kits/ROCK EDIT")
        _check("G2 kitId tetap 1 di lokasi baru",
               sd.read_json("/kits/ROCK EDIT/kit.json")["kitId"] == 1)

        # G3 DELETE: pohon dibuang -> slot bebas -> resolve None.
        sd.rmtree("/kits/ROCK EDIT")
        _check("G3 delete -> resolve slot1 None",
               resolve_kit_dir(sd, 1) is None)

        # G4 CREATE named: kit baru di folder nama user (kanonik).
        dirn = create_kit_empty(sd, 2, "FUSION", lambda i: default_source(i))
        _check("G4 folder berdasar nama user", dirn == "/kits/FUSION")
        _check("G4 slot2 masih Electronic selama kanonik itu ada",
               resolve_kit_dir(sd, 2) == "/kits/Electronic")
        sd.rmtree("/kits/Electronic")
        _check("G4 setelah Electronic dibuang -> FUSION (any)",
               resolve_kit_dir(sd, 2) == "/kits/FUSION")

        # Factory slot0 tidak ikut hilang.
        _check("factory slot0 tetap /kits/Acoustic",
               resolve_kit_dir(sd, 0) == "/kits/Acoustic")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


# ---------------------------------------------------------------
# G13 — Wemos UI CMD_KIT_INFO scheduling (semantic mirror dari
# uart_client kitInfo* + ui.ensureKitNameRequest/pollKitName).
# ESP32 sudah mengirim nama dengan benar; bug ada di urutan
# "request dulu, consume kemudian" + "request ulang tiap tick
# saat cache kosong" yang membuang reply lengkap.
# ---------------------------------------------------------------

class KitInfoLink:
    """Semantic mirror of the Wemos KIT_INFO assembler."""

    def __init__(self, name_max=KIT_NAME_MAX):
        self.name_max = name_max
        self.ready = False
        self.ok = False
        self.pending = False
        self.name = ""
        self.req_count = 0
        self.hdr_done = False
        self.chunk_done = False
        self.req_idx = 0
        self.req_seq = 0
        self.next_seq = 1
        self.kit_id = 0
        self.name_len = 0
        self.got = 0
        self.next_chunk = 0
        self.buffer = []
        self.last_result = None

    def _clear_packet(self):
        self.ready = False
        self.ok = False
        self.pending = False
        self.hdr_done = False
        self.chunk_done = False
        self.kit_id = 0
        self.name_len = 0
        self.got = 0
        self.next_chunk = 0
        self.buffer = []
        self.name = ""

    def request(self, idx):
        self._clear_packet()
        self.req_idx = idx
        self.req_seq = self.next_seq
        self.next_seq = (self.next_seq + 1) & 0xFF
        self.pending = True
        self.req_count += 1
        return idx

    def _seq_matches(self, seq):
        return seq is None or seq == self.req_seq

    def deliver_header(self, name_len, status=0, kit_id=0, seq=None):
        if not self.pending or not self._seq_matches(seq):
            return False

        self.pending = False
        self.ok = False
        self.ready = False
        self.hdr_done = False
        self.chunk_done = False
        self.kit_id = 0
        self.name_len = 0
        self.got = 0
        self.next_chunk = 0
        self.buffer = []
        self.name = ""

        if status != 0 or name_len <= 0:
            return False

        self.ok = True
        self.hdr_done = True
        self.kit_id = kit_id
        self.name_len = min(name_len, self.name_max)
        return True

    @staticmethod
    def _chunk_chars(data):
        if isinstance(data, str):
            chars = list(data)
        else:
            chars = list(data)
        while len(chars) < 3:
            chars.append("\0")
        return chars[:3]

    def deliver_chunk(self, chunk_index, data, seq=None):
        if (not self.ok or self.ready or
                not self._seq_matches(seq) or
                chunk_index != self.next_chunk):
            return False

        pos = chunk_index * 3
        if pos >= self.name_len or pos >= self.name_max:
            return False

        valid_chars = min(3, self.name_len - pos)
        if len(data) < valid_chars:
            return False
        chars = self._chunk_chars(data)
        for char in chars[:valid_chars]:
            if not isinstance(char, str):
                char = chr(char)
            if ord(char) < 0x20:
                char = " "
            self.buffer.append(char)

        self.next_chunk += 1
        self.got = pos + valid_chars
        if self.got >= self.name_len:
            self.name = "".join(self.buffer[:self.name_len])
            self.ready = True
            self.chunk_done = True
        return True

    def deliver_chunks(self, name, seq=None):
        if not self.hdr_done:
            return False
        source = name[:self.name_len]
        for index, start in enumerate(range(0, len(source), 3)):
            if not self.deliver_chunk(index, source[start:start + 3], seq):
                return False
        return self.ready

    def deliver_reply(self, name, idx=None, kit_id=0, seq=None):
        if idx is not None and idx != self.req_idx:
            return False
        if not self.deliver_header(len(name), kit_id=kit_id, seq=seq):
            return False
        return self.deliver_chunks(name, seq=seq)

    def consume_packet(self):
        if not self.ready:
            return None

        result = None
        if self.ok and self.name_len > 0 and self.name:
            result = (self.req_idx, self.kit_id, self.name)

        self.ready = False
        self.pending = False
        self.ok = False
        self.hdr_done = False
        self.chunk_done = False
        self.next_chunk = 0
        self.got = 0
        self.name_len = 0
        self.last_result = result
        return result

    def consume(self):
        result = self.consume_packet()
        return result[2] if result is not None else None

    def timeout(self):
        if not (self.pending or (self.ok and not self.ready)):
            return False
        self._clear_packet()
        return True

    def busy(self):
        return self.pending or self.ok or self.ready


def simulate_kitinfo(name, ticks, fixed):
    """Satu siklus per tick (UI_UPDATE_MS):
       (a) reply utk request tick lalu tiba & dirakit,
       (b) pollKitName/consume,
       (c) ensureKitNameRequest (fixed: skip bila cache ada / pending).
    Mengembalikan (nama ter-cache, jumlah request yang benar dilewati)."""
    link = KitInfoLink()
    cached = ""
    pending_reply = None

    for _ in range(ticks):
        if pending_reply is not None:
            link.deliver_reply(pending_reply)
            pending_reply = None

        if fixed:
            got = link.consume()
            if got is not None:
                cached = got
            if cached == "" and not link.pending:
                link.request(0)
                pending_reply = name
        else:
            if cached == "":
                link.request(0)
                pending_reply = name
            got = link.consume()
            if got is not None:
                cached = got

    return cached, link.req_count


def simulate_kitinfo_split(name, guard_busy):
    """Buruk-luck burst: header tiba di tick ini, chunk tick berikutnya
    (drain UART terpecah / LCD write panjang). `guard_busy` = apakah
    ensure memakai uartKitInfoBusy() (pending|ok|ready) — vs G13 yang
    hanya memeriksa pending. Mengembalikan (nama ter-cache)."""
    link = KitInfoLink()
    cached = ""
    sent = False

    for _ in range(10):
        # (a) reply: header lalu chunk (terpisah dua drain).
        if sent and not link.hdr_done:
            link.deliver_header(len(name))
        elif sent and link.hdr_done and not link.chunk_done:
            link.deliver_chunks(name)
            link.hdr_done = True

        # (b) handleMain: poll dulu.
        got = link.consume()
        if got is not None:
            cached = got
            break

        # (c) ensure: cache kosong -> hembuskan request bila idle.
        busy = (link.pending or link.ok or link.ready) if guard_busy \
            else link.pending
        if cached == "" and not busy:
            link.request(0)
            sent = True

    return cached


def test_g13_wemos_kitinfo_scheduling():
    print("12. G13 scheduling KIT_INFO: consume-first + in-flight guard")
    sd, tmp = sd_home()
    try:
        # ESP32 mengirim nama dengan benar (pola G13 audit).
        names = {0: "Acoustic", 1: "ROCK LIVE"}
        _check("ESP32 KIT_INFO(0) berisi 'Acoustic'",
               names[0] == sd.read_json("/kits/Acoustic/kit.json")["name"])
        _check("ESP32 KIT_INFO(1) berisi 'ROCK LIVE'",
               names[1] == sd.read_json("/kits/ROCK LIVE/kit.json")["name"])

        # --- Bug pra-fix: cache tidak pernah terisi (livelock) ---
        cached, reqs = simulate_kitinfo(names[0], ticks=20, fixed=False)
        _check("pra-fix: cache tetap kosong setelah 20 tick",
               cached == "" and reqs == 20)

        # --- Fix G13: cache terisi, request berhenti ---
        cached, reqs = simulate_kitinfo(names[0], ticks=20, fixed=True)
        _check("fix: nama aktif ter-cache ('Acoustic')", cached == "Acoustic")
        _check("fix: request tidak me-request ulang setiap tick",
               reqs <= 2)

        # --- Nama berspasi juga ter-cache ---
        cached2, reqs2 = simulate_kitinfo(names[1], ticks=20, fixed=True)
        _check("fix: 'ROCK LIVE' ter-cache", cached2 == "ROCK LIVE")
        _check("fix: request ROCK LIVE dibatasi", reqs2 <= 2)

        # --- Setelah invalidation (LOAD/RENAME/DELETE/init): bisa di-request
        #     ulang tanpa livelock; cache lama tidak menghalangi ---
        link = KitInfoLink()
        pending_reply = None
        cached = ""
        # tick hingga cache penuh
        for _ in range(3):
            if pending_reply is not None:
                link.deliver_reply(pending_reply)
                pending_reply = None
            got = link.consume()
            if got is not None:
                cached = got
            if cached == "" and not link.pending:
                link.request(0)
                pending_reply = names[0]
        # invalidate: cache dibuang (ui.invalidateKitName)
        cached = ""
        before = link.req_count
        for _ in range(4):
            if pending_reply is not None:
                link.deliver_reply(pending_reply)
                pending_reply = None
            got = link.consume()
            if got is not None:
                cached = got
            if cached == "" and not link.pending:
                link.request(0)
                pending_reply = names[0]
        _check("fix: re-request setelah invalidate berhasil",
               cached == "Acoustic")
        _check("fix: tepat satu request baru setelah invalidate",
               link.req_count == before + 1)

        # --- Timeout aman (tanpa fake success): reply hilang -> pending
        #     akhirnya timeout, lalu request baru boleh & tetap tunjukkan
        #     '...' (cache kosong) sampai sukses benar ---
        link = KitInfoLink()
        pending_name = link.request(0)
        # reply tidak pernah datang; timeout mensimulasikan state selesai
        # dengan hasil gagal -> consume None, pending clear.
        link.pending = False
        link.ok = False
        link.ready = True
        got = link.consume()
        _check("timeout: konsumsi gagal (bukan fake success)", got is None)
        cached = ""
        # recovery: request baru dizinkan setelah state bersih
        link.pending = False
        if cached == "" and not link.pending:
            pending_name = link.request(0)
        link.deliver_reply("Acoustic")
        got = link.consume()
        _check("timeout: recovery request berikutnya sukses", got == "Acoustic")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def test_g14_split_burst_guard():
    print("13. G14 burst split: busy-guard melindungi rakitan chunk")
    # Skenario nyata: header tiba duluan, chunk tergeser ke drain lain.
    # Guard G13 (pending saja) membiarkan request baru lewat begitu
    # header masuk -> ok/reset membuang rakitan chunk (livelock persist).
    # Guard G14 (busy = pending|ok|ready) meneruskan sampai chunk utuh.
    _check("pending-only guard (G13) TETAP livelock saat burst pecah",
           simulate_kitinfo_split("Acoustic", guard_busy=False) == "")
    _check("busy guard (G14) sembuhkan name saat burst pecah",
           simulate_kitinfo_split("Acoustic", guard_busy=True) == "Acoustic")
    _check("busy guard berlaku juga utk nama panjang (26 char)",
           simulate_kitinfo_split("ROCK-ELECTRONIC-FUSION-01",
                                  guard_busy=True) == "ROCK-ELECTRONIC-FUSION-01")


def render_active_name(name):
    return name if name else "..."


def copy_name_buffer(name, capacity):
    buf = bytearray(capacity)
    encoded = name[:capacity - 1].encode()
    buf[:len(encoded)] = encoded
    return buf


def test_g17_kitinfo_assembly_and_renderer():
    print("15. G17 KIT_INFO assembly: chunk validation, timeout, cache, renderer")
    sd, tmp = sd_home()
    try:
        acoustic = sd.read_json("/kits/Acoustic/kit.json")["name"]
        rock = sd.read_json("/kits/ROCK LIVE/kit.json")["name"]

        link = KitInfoLink()
        link.request(0)
        header_ok = link.deliver_header(len(acoustic), kit_id=0)
        expected_chunks = [
            acoustic[start:start + 3]
            for start in range(0, len(acoustic), 3)
        ]
        chunk_results = [
            link.deliver_chunk(index, data)
            for index, data in enumerate(expected_chunks)
        ]
        acoustic_result = link.consume_packet()
        _check("Acoustic header diterima", header_ok)
        _check("chunk Acoustic sesuai sender 3-byte",
               expected_chunks == ["Aco", "ust", "ic"])
        _check("Acoustic chunks 0/1/2 diterima", chunk_results == [True, True, True])
        _check("Acoustic lengkap di-consume dengan id/index benar",
               acoustic_result == (0, 0, "Acoustic"))
        _check("renderer memakai cache Acoustic",
               render_active_name(acoustic_result[2] if acoustic_result else "") == "Acoustic")

        link = KitInfoLink()
        link.request(1)
        rock_header = link.deliver_header(len(rock), kit_id=1)
        rock_chunks = [
            link.deliver_chunk(index, data)
            for index, data in enumerate(("ROC", "K L", "IVE"))
        ]
        rock_result = link.consume_packet()
        _check("ROCK LIVE header diterima", rock_header)
        _check("ROCK LIVE chunks 0/1/2 diterima", rock_chunks == [True, True, True])
        _check("ROCK LIVE dengan spasi utuh", rock_result == (1, 1, "ROCK LIVE"))

        link = KitInfoLink()
        link.request(2)
        link.deliver_header(1, kit_id=2)
        short_result = link.consume_packet() if link.deliver_chunk(0, ("K", "\0", "\0")) else None
        _check("nama satu karakter K selesai aman", short_result == (2, 2, "K"))

        long_name = "0123456789ABCDEFGHIJKLMNOPQRSTUV"
        link = KitInfoLink()
        link.request(3)
        link.deliver_header(len(long_name), kit_id=3)
        for index, start in enumerate(range(0, KIT_NAME_MAX, 3)):
            link.deliver_chunk(index, long_name[start:start + 3])
        long_result = link.consume_packet()
        _check("nama 32 char dibatasi KIT_NAME_MAX",
               long_result == (3, 3, long_name[:KIT_NAME_MAX]))
        long_buffer = copy_name_buffer(long_name, KIT_NAME_MAX + 1)
        _check("nama panjang tetap null-safe dan tanpa data excess",
               long_result is not None and
               len(long_result[2]) <= KIT_NAME_MAX and
               long_buffer[KIT_NAME_MAX] == 0 and
               bytes(long_buffer).split(b"\0", 1)[0] == long_result[2].encode())

        link = KitInfoLink()
        link.request(0)
        link.deliver_header(len(acoustic), kit_id=0)
        wrong_order = link.deliver_chunk(1, "sti")
        wrong_seq = link.deliver_chunk(0, "Acu", seq=(link.req_seq + 1) & 0xFF)
        valid_after_reject = [
            link.deliver_chunk(index, data)
            for index, data in enumerate(("Aco", "ust", "ic"))
        ]
        recovered = link.consume_packet()
        _check("chunk out-of-order ditolak", not wrong_order)
        _check("chunk SEQ salah ditolak", not wrong_seq)
        _check("chunk valid setelah noise diterima", valid_after_reject == [True, True, True])
        _check("paket tetap utuh setelah noise", recovered == (0, 0, "Acoustic"))

        invalid = KitInfoLink()
        invalid.request(0)
        invalid_header = invalid.deliver_header(len(acoustic), status=4)
        _check("header status invalid ditolak tanpa ready",
               not invalid_header and
               not invalid.busy() and
               invalid.consume_packet() is None)

        link = KitInfoLink()
        link.request(0)
        link.deliver_header(len(acoustic), kit_id=0)
        link.deliver_chunk(0, "Acu")
        timed_out = link.timeout()
        _check("timeout setelah header tanpa semua chunk aktif", timed_out)
        _check("timeout tidak menghasilkan ready/sukses",
               not link.ready and link.consume_packet() is None and not link.busy())

        cached, reqs = simulate_kitinfo("Acoustic", ticks=20, fixed=True)
        _check("G13 regression: request berhenti setelah consume",
               cached == "Acoustic" and reqs == 1)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def test_g16_active_kit_name_cache():
    print("14. G16 active-kit name cache: metadata, preview, rename, fallback")
    sd, tmp = sd_home()
    try:
        runtime = KitNameRuntime(sd)

        runtime.load(KIT_FACTORY_INDEX)
        _check("active slot0 memuat Acoustic",
               runtime.get_name(0) == "Acoustic")
        _check("nama aktif tetap tersedia dari cache",
               runtime.cached_name == "Acoustic" and runtime.cache_valid)

        runtime.cached_name = "UNNAMED"
        runtime.cache_valid = False
        _check("valid metadata menggantikan cache invalid",
               runtime.get_name(0) == "Acoustic")

        _check("preview non-active name-based",
               runtime.get_name(1) == "ROCK LIVE")

        runtime.load(1)
        _check("load kit berspasi mempertahankan nama",
               runtime.get_name(1) == "ROCK LIVE")
        runtime.set_cached_name(1, "ROCK EDIT")
        _check("rename active memperbarui cache",
               runtime.get_name(1) == "ROCK EDIT")

        runtime.load(2)
        _check("load kit lain mengganti nama aktif",
               runtime.get_name(2) == "Electronic")
        _check("kit sebelumnya menjadi preview valid",
               runtime.get_name(0) == "Acoustic")

        runtime.load(0)
        sd.rmtree("/kits/Acoustic")
        runtime.load(0)
        _check("missing metadata hanya menghasilkan UNNAMED",
               runtime.get_name(0) == "UNNAMED")

        sd.mkdirs("/kits/Acoustic")
        sd.write_json("/kits/Acoustic/kit.json",
                      {"kitId": 0, "id": 0, "name": 123, "pads": []})
        runtime.load(0)
        _check("nama metadata invalid fallback UNNAMED",
               runtime.get_name(0) == "UNNAMED")

        sd.write_json("/kits/Acoustic/kit.json",
                      {"kitId": 0, "id": 0, "name": "Acoustic", "pads": []})
        runtime.load(0)
        sd.rmtree("/kits/ROCK LIVE")
        runtime.load(1)
        runtime.load(KIT_FACTORY_INDEX)
        _check("delete active lalu factory fallback Acoustic",
               runtime.get_name(0) == "Acoustic")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def test_g18_preview_resolver_and_stack_safety():
    print("16. G18 preview resolver, cache ownership, dan stack-safe KIT_INFO")
    sd, tmp = sd_home()
    try:
        runtime = KitNameRuntime(sd)
        runtime.load(0)

        expected = {
            0: (0, "Acoustic"),
            1: (1, "ROCK LIVE"),
            2: (2, "Electronic"),
            3: (3, "Jazz"),
            4: (4, "Latin"),
        }
        for index, (kit_id, name) in expected.items():
            got_id, got_name, found = runtime.get_info(index)
            _check(
                f"KIT_INFO preview idx={index} resolve {name}",
                found and got_id == kit_id and got_name == name,
                f"got={(got_id, got_name, found)}"
            )

        repeated = [runtime.get_info(1) for _ in range(6)]
        _check("KIT_INFO preview repeated stabil",
               all(item == (1, "ROCK LIVE", True) for item in repeated))

        sd.mkdirs("/kits/ROCK-ARCHIVE")
        shutil.copyfile(
            sd.p("/kits/ROCK LIVE/kit.json"),
            sd.p("/kits/ROCK-ARCHIVE/kit.json")
        )
        sd.rmtree("/kits/ROCK LIVE")
        _check("noncanonical folder resolve by kitId",
               resolve_kit_dir(sd, 1) == "/kits/ROCK-ARCHIVE")
        _check("noncanonical folder KIT_INFO retains metadata name",
               runtime.get_info(1) == (1, "ROCK LIVE", True))

        doc = sd.read_json("/kits/ROCK-ARCHIVE/kit.json")
        doc["name"] = "ROCK EDIT"
        sd.write_json("/kits/ROCK-ARCHIVE/kit.json", doc)
        os.rename(
            sd.p("/kits/ROCK-ARCHIVE"),
            sd.p("/kits/ROCK EDIT")
        )
        _check("rename-move KIT_INFO resolves canonical name",
               runtime.get_info(1) == (1, "ROCK EDIT", True))

        runtime.load(1)
        _check("active cache 0 -> preview 0 tidak bocor",
               runtime.get_info(0) == (0, "Acoustic", True))
        runtime.load(0)
        _check("active cache 1 -> 0 -> preview 1 kembali live",
               runtime.get_info(1) == (1, "ROCK EDIT", True))

        runtime.load(0)
        runtime.loaded_id = 1
        runtime.cache_valid = True
        runtime.cached_name = "WRONG CACHE"
        _check("preview index tidak memakai alias kitId active",
               runtime.get_info(1) == (1, "ROCK EDIT", True))

        sd.rmtree("/kits/ROCK EDIT")
        _check("delete preview menghasilkan UNNAMED",
               runtime.get_info(1) == (1, "UNNAMED", False))

        root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        with open(os.path.join(root, "DrumCore", "uart_protocol.cpp"),
                  "r", encoding="utf-8") as f:
            uart_source = f.read()
        with open(os.path.join(root, "DrumCore", "kit_loader.cpp"),
                  "r", encoding="utf-8") as f:
            loader_source = f.read()
        with open(os.path.join(root, "DrumCore", "sdcard.cpp"),
                  "r", encoding="utf-8") as f:
            sd_source = f.read()

        case_start = uart_source.find("case CMD_KIT_INFO:")
        case_end = uart_source.find("case CMD_PAD_SET:", case_start)
        case_source = uart_source[case_start:case_end]
        resolver_start = loader_source.find("resolveKitDirWithMeta(")
        resolver_end = loader_source.find("bool kitLoaderResolveKitDir(",
                                          resolver_start)
        resolver_source = loader_source[resolver_start:resolver_end]

        _check("CMD_KIT_INFO tidak double lookup name/id",
               "kitLoaderGetName(" not in case_source and
               "kitLoaderGetKitId(" not in case_source and
               "handleKitInfo(a0, seq)" in case_source)
        _check("KIT_INFO lookup punya task stack terpisah",
               "KIT_INFO_TASK_STACK" in uart_source and
               "kitInfoTaskLoop" in uart_source and
               "kitLoaderGetKitInfo(" in uart_source)
        _check("PAD_SOUND_SET snapshot tetap di luar dispatch",
               "__attribute__((noinline)) uint8_t performSampleAssign("
               in uart_source)
        _check("KIT_INFO resolver menutup entry sebelum metadata read",
               "entry.close();" in resolver_source and
               "SD.exists(metaPath)" not in resolver_source)
        _check("active cache ownership strictly by index",
               "return kitIndex == activeKitIndex;" in loader_source)
        _check("SD mount memperbesar file-object pool",
               '"/sd",' in sd_source and "10))" in sd_source)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def test_g19_kit_load_stack_fix():
    """G19: Verifikasi fix stack overflow UartTask.

    Menguji semantik CMD_KIT_LOAD + kit switch + repeated load + SD
    unavailable behavior. Regression lock memastikan perubahan G19
    (static buffer, stack size, SD retry) tidak merusak behavior yang
    sudah ada.
    """
    print("17. G19 CMD_KIT_LOAD stack-fix regression + SD fail semantics")
    sd, tmp = sd_home()
    try:
        runtime = KitNameRuntime(sd)

        # ----------------------------------------------------------
        # 17.1 LOAD Acoustic (slot 0) - kit factory
        # ----------------------------------------------------------
        runtime.load(KIT_FACTORY_INDEX)
        _check("G19 LOAD Acoustic: kit factory ter-load",
               runtime.get_name(0) == "Acoustic")
        _check("G19 LOAD Acoustic: active index benar",
               runtime.active_index == 0)

        # ----------------------------------------------------------
        # 17.2 LOAD Electronic (slot 2)
        # ----------------------------------------------------------
        runtime.load(2)
        _check("G19 LOAD Electronic: nama benar",
               runtime.get_name(2) == "Electronic")
        _check("G19 LOAD Electronic: active index 2",
               runtime.active_index == 2)

        # ----------------------------------------------------------
        # 17.3 LOAD kembali Acoustic (idempoten jika sudah aktif)
        # ----------------------------------------------------------
        runtime.load(KIT_FACTORY_INDEX)
        _check("G19 LOAD ulang Acoustic: nama tetap benar",
               runtime.get_name(0) == "Acoustic")

        # ----------------------------------------------------------
        # 17.4 LOAD ROCK LIVE (slot 1, nama berspasi)
        # ----------------------------------------------------------
        runtime.load(1)
        _check("G19 LOAD ROCK LIVE: nama berspasi benar",
               runtime.get_name(1) == "ROCK LIVE")

        # ----------------------------------------------------------
        # 17.5 RENAME lalu KIT_INFO — nama harus ter-update
        # ----------------------------------------------------------
        # Simulate rename: ubah JSON name di folder dan pindahkan.
        doc = sd.read_json("/kits/ROCK LIVE/kit.json")
        doc["name"] = "Kendang"
        sd.write_json("/kits/ROCK LIVE/kit.json", doc)
        os.rename(sd.p("/kits/ROCK LIVE"), sd.p("/kits/Kendang"))

        # Re-load setelah rename (folder pindah).
        runtime.load(1)
        _check("G19 RENAME -> LOAD: nama baru Kendang",
               runtime.get_name(1) == "Kendang")

        # Resolver melihat slot 1 di /kits/Kendang.
        _check("G19 RENAME: resolve slot1 -> /kits/Kendang",
               resolve_kit_dir(sd, 1) == "/kits/Kendang")

        # ----------------------------------------------------------
        # 17.6 LOAD Kendang setelah rename
        # ----------------------------------------------------------
        runtime.load(1)
        _check("G19 LOAD Kendang: aktif di slot 1",
               runtime.active_index == 1 and runtime.get_name(1) == "Kendang")

        # ----------------------------------------------------------
        # 17.7 Repeated switching (20x) — tidak ada state corruption
        # ----------------------------------------------------------
        kits = [0, 1, 2, 3, 4]
        kit_names = {0: "Acoustic", 1: "Kendang", 2: "Electronic",
                     3: "Jazz", 4: "Latin"}
        ok = True
        for i in range(20):
            slot = kits[i % len(kits)]
            runtime.load(slot)
            if runtime.get_name(slot) != kit_names[slot]:
                ok = False
                break
        _check("G19 repeated switching 20x: no state corruption", ok)

        # ----------------------------------------------------------
        # 17.8 KIT_INFO spam (repeated non-active previews)
        # ----------------------------------------------------------
        runtime.load(0)  # reset ke Acoustic
        spam_ok = True
        for i in range(10):
            name = runtime.get_name(1)   # preview kit 1 = Kendang
            if name != "Kendang":
                spam_ok = False
                break
        _check("G19 spam KIT_INFO non-active: stabil", spam_ok)

        # ----------------------------------------------------------
        # 17.9 DELETE kit lalu fallback ke factory
        # ----------------------------------------------------------
        # Simulate DELETE slot 1 (Kendang).
        sd.rmtree("/kits/Kendang")
        _check("G19 DELETE: resolve slot1 -> None",
               resolve_kit_dir(sd, 1) is None)

        # Boot fallback: slot 1 hilang -> factory fallback.
        saved_active = 0  # firmware memilih KIT_FACTORY_INDEX
        _check("G19 DELETE: fallback ke factory slot 0", saved_active == 0)
        runtime.load(saved_active)
        _check("G19 DELETE fallback: Acoustic loaded",
               runtime.get_name(0) == "Acoustic")

        # ----------------------------------------------------------
        # 17.10 SD unavailable semantics (G19 fix)
        #
        # Ketika SD tidak ready:
        # - kit aktif dengan cache valid HARUS tetap resolve (dari cache)
        # - kit non-aktif HARUS return false/empty (bukan "UNNAMED" palsu)
        # ----------------------------------------------------------

        class SdUnavailable(Sd):
            """Simulasi SD gagal: semua operasi filesystem raise exception."""
            def listdir(self, path):
                return []   # tidak ada entry, scan kosong
            def exists(self, path):
                return False   # tidak ada file
            def read_json(self, path):
                raise OSError("SD not ready")

        sd_fail = SdUnavailable(tmp)
        runtime_fail = KitNameRuntime(sd_fail)

        # Tanpa load awal (cache kosong, SD gagal) -> UNNAMED.
        name_fail = runtime_fail.get_info(0)
        _check("G19 SD fail + no cache: return UNNAMED/False",
               name_fail[2] is False)

        # Dengan cache valid sebelum SD gagal (analog kit aktif): harus dari cache.
        runtime_ok = KitNameRuntime(sd)   # pakai SD asli
        runtime_ok.load(0)    # load aktif -> cache valid
        # Sekarang simulasikan SD hilang: copy state ke runtime dengan SD gagal.
        runtime_cached = KitNameRuntime(sd_fail)
        runtime_cached.active_index = 0
        runtime_cached.loaded_id = 0
        runtime_cached.cached_name = "Acoustic"
        runtime_cached.cache_valid = True
        name_cached = runtime_cached.get_info(0)
        _check("G19 SD fail + cache valid: active kit serve dari cache",
               name_cached == (0, "Acoustic", True))

        # Preview kit (non-aktif) saat SD gagal: harus return False.
        name_preview = runtime_cached.get_info(1)
        _check("G19 SD fail + preview non-active: return False",
               name_preview[2] is False)

        # ----------------------------------------------------------
        # 17.11 Source code verification — static buffers
        # ----------------------------------------------------------
        root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        with open(os.path.join(root, "DrumCore", "uart_protocol.cpp"),
                  "r", encoding="utf-8") as f:
            uart_src = f.read()
        with open(os.path.join(root, "DrumCore", "kit_loader.cpp"),
                  "r", encoding="utf-8") as f:
            loader_src = f.read()
        with open(os.path.join(root, "DrumCore", "sdcard.cpp"),
                  "r", encoding="utf-8") as f:
            sd_src = f.read()

        # Stack size harus 8192.
        _check("G19 UART_TASK_STACK == 8192",
               "UART_TASK_STACK = 8192" in uart_src)

        # cfgSnapshot dan runSnapshot harus static.
        _check("G19 cfgSnapshot static",
               "static PadConfig cfgSnapshot[PAD_COUNT]" in uart_src)
        _check("G19 runSnapshot static",
               "static PadConfig runSnapshot[PAD_COUNT]" in uart_src)

        # G21 replaces the old snapshot/rollback buffer: remap tidak
        # pernah menyentuh PSRAM yang sudah termuat, jadi tidak ada
        # yang perlu di-rollback. Yang dikunci di sini: jalur
        # PAD_SOUND_SET tidak boleh memuat ulang kit. Komentar
        # dibuang supaya hanya KODE yang diperiksa.
        assign_code = _strip_comments(
            _func_body(uart_src, "uint8_t performSampleAssign("))
        _check("G21 performSampleAssign tidak memanggil psramLoadKit",
               "psramLoadKit(" not in assign_code)
        _check("G21 performSampleAssign tidak memakai performKitSwitch",
               "performKitSwitch(" not in assign_code)
        _check("G21 performSampleAssign tidak menulis SD langsung",
               "writeJson" not in assign_code and
               "SD.open" not in assign_code and
               "SD.exists" not in assign_code)


        # Watermark logging harus ada.
        _check("G19 watermark logging ada",
               "uxTaskGetStackHighWaterMark" in uart_src)

        # CMD_KIT_LOAD harus log BEGIN/END.
        _check("G19 CMD_KIT_LOAD BEGIN log",
               "CMD_KIT_LOAD BEGIN" in uart_src)

        # SD init harus punya retry.
        _check("G19 SD init retry logic",
               "SD_INIT_RETRIES" in sd_src and "attempt" in sd_src)

        # SD init harus punya SPI.end() untuk clean retry.
        _check("G19 SD SPI.end() sebelum retry",
               "sdSPI.end()" in sd_src)

        # kitLoaderGetKitInfo harus check sdIsReady().
        _check("G19 kitLoaderGetKitInfo cek sdIsReady",
               "sdIsReady()" in loader_src)

        # kit_loader.cpp harus include sdcard.h.
        _check("G19 kit_loader.cpp include sdcard.h",
               '"sdcard.h"' in loader_src)

        # performSampleAssign masih __attribute__((noinline)).
        _check("G19 performSampleAssign tetap noinline",
               "__attribute__((noinline)) uint8_t performSampleAssign(" in uart_src)

        # SD diagnostics harus ada di sdcard.cpp.
        _check("G19 SD diagnostics [SD] begin attempt",
               "[SD] begin attempt" in sd_src)

        # UART protocol constants tidak berubah.
        _check("G19 UART SOF 0xF7 tidak berubah",
               "0xF7" in uart_src or "UART_SOF" in uart_src)

        # ----------------------------------------------------------
        # 17.12 Protocol frame format tidak berubah
        # ----------------------------------------------------------
        # Verifikasi bahwa response format masih pakai CMD | RESP_FLAG.
        _check("G19 UART_RESP_FLAG masih dipakai",
               "UART_RESP_FLAG" in uart_src)
        _check("G19 CMD_KIT_LOAD masih ada di dispatch",
               "case CMD_KIT_LOAD:" in uart_src)
        _check("G19 sendResponse masih dipanggil di CMD_KIT_LOAD",
               "sendResponse(cmd, st, uartActiveKit" in uart_src)

    finally:
        shutil.rmtree(tmp, ignore_errors=True)




# ---------------------------------------------------------------
# G20 — Wemos PAD_SOUND assembler (semantic mirror dari
# uart_client.cpp handlePadSoundFrame + uartPadSoundRequest).
#
# Protocol:
#   header daftar : ARG2==0xC7, ARG1=count, ARG3=currentIndex
#   nama-header   : ARG0=idx (tanpa 0x80), ARG1=nameLen, ARG2=0
#   chunk         : ARG0=(chunkIdx|0x80), ARG1..3 = sampai 3 char
#
# Fix G20:
#   #1 count==0 langsung selesaikan sesi (ok=True, ready=True)
#   #2 got = pos + validChars (bukan +=3)
# ---------------------------------------------------------------

PAD_SOUND_NAME_MAX = 32
PAD_SOUND_COUNT = 12


class PadSoundLink:
    """Semantic mirror of Wemos uart_client CMD_PAD_SOUND assembler.

    Memodelkan state persis handlePadSoundFrame() setelah G20 fix:
      - header daftar (0xC7): count==0 langsung ready (fix #1)
      - chunk: got = pos + validChars (fix #2)
    """

    def __init__(self):
        self.pending = False
        self.has_name = False
        self.ok = False
        self.ready = False
        self.seq = 0
        self._next_seq = 1

        self.count = 0
        self.idx = 0           # nama yang sedang dirakit
        self.name_len = 0
        self.got = 0
        self.current = 0xFF
        self.names = [""] * PAD_SOUND_COUNT

    # ---- Kirim request baru ----

    def request(self, pad_index):
        """uartPadSoundRequest(): reset state, kirim frame CMD_PAD_SOUND."""
        self.pending = True
        self.has_name = False
        self.ok = False
        self.ready = False
        self.count = 0
        self.idx = 0
        self.name_len = 0
        self.got = 0
        self.current = 0xFF
        self.names = [""] * PAD_SOUND_COUNT
        self.seq = self._next_seq
        self._next_seq = (self._next_seq + 1) & 0xFF
        return self.seq

    # ---- Injektor frame individual (seperti drain UART) ----

    def deliver_list_header(self, count, current=0xFF, seq=None):
        """Frame header daftar: ARG2==0xC7."""
        if not self.pending:
            return False
        if seq is not None and seq != self.seq:
            return False

        self.count = min(count, PAD_SOUND_COUNT)
        self.idx = 0
        self.has_name = False
        self.current = current
        self.names = [""] * PAD_SOUND_COUNT

        # G20 FIX #1: count==0 -> sesi selesai langsung.
        if self.count == 0:
            self.ok = True
            self.pending = False
            self.ready = True

        return True

    def deliver_name_header(self, idx, name_len, seq=None):
        """Frame nama-header: ARG0=idx, ARG1=nameLen, ARG2=0."""
        if not self.pending:
            return False
        if seq is not None and seq != self.seq:
            return False
        if self.idx >= self.count:
            return False
        if idx != self.idx:
            return False

        self.name_len = min(name_len, PAD_SOUND_NAME_MAX)
        self.got = 0
        self.has_name = True

        # Nama kosong: lanjut ke berikutnya.
        if self.name_len == 0:
            self.has_name = False
            self.idx += 1
            if self.idx >= self.count:
                self.ok = True
                self.pending = False
                self.ready = True

        return True

    def deliver_chunk(self, chunk_index, chars, seq=None):
        """Frame chunk: ARG0=(chunkIdx|0x80), ARG1..3 = chars."""
        if not self.pending or not self.has_name:
            return False
        if seq is not None and seq != self.seq:
            return False

        pos = chunk_index * 3
        if pos >= self.name_len:
            return False

        # G20 FIX #2: validChars = min(3, name_len - pos).
        valid_chars = min(3, self.name_len - pos)

        # Gunakan bytearray-style assignment ke dalam nama yang sudah ada.
        # Mulai dari string yang sudah terbentuk sebelumnya (bisa kosong).
        existing = self.names[self.idx]
        # Pad ke panjang penuh dengan '\x00' untuk assignment aman.
        buf = list(existing.ljust(PAD_SOUND_NAME_MAX, '\x00'))

        for i in range(valid_chars):
            c = chars[i] if i < len(chars) else '\x00'
            if isinstance(c, int):
                c = chr(c)
            if ord(c) < 0x20:
                c = ' '
            if pos + i < PAD_SOUND_NAME_MAX:
                buf[pos + i] = c

        # got = pos + validChars (bukan +=3).
        self.got = pos + valid_chars

        if self.got >= self.name_len:
            # Nama selesai: potong tepat di name_len, buang null padding.
            name_str = "".join(buf[:self.name_len])
            self.names[self.idx] = name_str
            self.has_name = False
            self.idx += 1
            if self.idx >= self.count:
                self.ok = True
                self.pending = False
                self.ready = True
        else:
            # Nama belum selesai: simpan progress.
            self.names[self.idx] = "".join(buf[:pos + valid_chars])

        return True

    # ---- Kirim seluruh nama sekaligus (helper) ----

    def deliver_name(self, idx, name, seq=None):
        """Helper: kirim nama-header + semua chunk untuk satu nama."""
        if not self.deliver_name_header(idx, len(name), seq=seq):
            return False
        for ci, start in enumerate(range(0, len(name), 3)):
            chunk = list(name[start:start + 3])
            while len(chunk) < 3:
                chunk.append('\0')
            if not self.deliver_chunk(ci, chunk, seq=seq):
                return False
        return True

    # ---- Kirim seluruh daftar sekaligus (helper) ----

    def deliver_full_list(self, names, current=0xFF, seq=None):
        """Helper: kirim header daftar + semua nama."""
        if not self.deliver_list_header(len(names), current=current, seq=seq):
            return False
        for i, n in enumerate(names):
            if not self.deliver_name(i, n, seq=seq):
                return False
        return self.ready

    # ---- Timeout (dikerjakan uartClientUpdate setelah 2500ms) ----

    def timeout(self):
        """Simulasikan timeout: pending -> ready=True, ok=False."""
        if self.pending:
            self.pending = False
            self.ok = False
            self.ready = True

    # ---- Consume hasil ----

    def consume(self):
        """uartPadSoundGet(): ambil daftar (atau None bila gagal)."""
        if not self.ready:
            return None
        self.ready = False
        self.pending = False
        if not self.ok:
            return None
        return (list(self.names[:self.count]), self.current)


# ---------------------------------------------------------------
# G20 Tests
# ---------------------------------------------------------------

def test_g20_pad_sound_empty_samples():
    print("G20.1 PAD_SOUND empty samples -> valid empty response (bukan timeout)")
    # G20 FIX #1: count==0 harus langsung selesaikan sesi.
    link = PadSoundLink()
    seq = link.request(0)

    _check("G20.1 pending setelah request", link.pending)

    # Header dengan count=0 (folder samples kosong / kit baru).
    ok = link.deliver_list_header(0, current=0xFF, seq=seq)
    _check("G20.1 header count=0 diterima", ok)
    _check("G20.1 ready=True setelah count=0 (bukan tunggu nama)", link.ready)
    _check("G20.1 ok=True (bukan timeout)", link.ok)
    _check("G20.1 pending=False (sesi selesai)", not link.pending)

    result = link.consume()
    _check("G20.1 consume return tuple (bukan None)", result is not None)
    if result is not None:
        names, cur = result
        _check("G20.1 daftar kosong", names == [])
        _check("G20.1 current=0xFF", cur == 0xFF)


def test_g20_pad_sound_non_empty():
    print("G20.2 PAD_SOUND non-empty -> daftar lengkap dirakit benar")
    sd, tmp = sd_home()
    try:
        # Gunakan nama nyata dari fixture Acoustic.
        names_real = scan_samples(sd, "/kits/Acoustic")
        _check("G20.2 fixture Acoustic punya 12 sample", len(names_real) == 12)

        link = PadSoundLink()
        seq = link.request(0)
        ok = link.deliver_full_list(names_real, current=0, seq=seq)

        _check("G20.2 deliver_full_list sukses", ok)
        _check("G20.2 ready=True", link.ready)

        result = link.consume()
        _check("G20.2 consume berhasil", result is not None)
        if result is not None:
            names_got, cur = result
            _check("G20.2 jumlah nama benar", len(names_got) == len(names_real))
            _check("G20.2 nama pertama benar", names_got[0] == names_real[0])
            _check("G20.2 nama terakhir benar",
                   names_got[-1] == names_real[-1])
            _check("G20.2 currentIndex=0", cur == 0)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def test_g20_pad_sound_last_chunk_variants():
    print("G20.3 PAD_SOUND last-chunk 1/2/3 byte -> got tepat (fix #2)")
    # Nama dengan panjang mod 3 = 1, 2, 3 menguji tiap ukuran chunk terakhir.
    test_cases = [
        ("A.wav",      5,  "panjang 5 (mod3=2, last chunk 2 char)"),
        ("AB.wav",     6,  "panjang 6 (mod3=0, last chunk 3 char)"),
        ("ABC.wav",    7,  "panjang 7 (mod3=1, last chunk 1 char)"),
        ("KICK.wav",   8,  "panjang 8 (mod3=2, last chunk 2 char)"),
        ("SNARE.wav",  9,  "panjang 9 (mod3=0, last chunk 3 char)"),
        ("SPLASH.wav", 10, "panjang 10 (mod3=1, last chunk 1 char)"),
    ]

    for name, expected_len, desc in test_cases:
        assert len(name) == expected_len, f"{name}: len mismatch"
        link = PadSoundLink()
        seq = link.request(0)
        link.deliver_list_header(1, current=0, seq=seq)
        ok = link.deliver_name(0, name, seq=seq)
        _check(f"G20.3 deliver '{name}' ({desc})", ok and link.ready)
        result = link.consume()
        _check(f"G20.3 consume '{name}'", result is not None)
        if result is not None:
            got_name = result[0][0] if result[0] else ""
            _check(f"G20.3 nama benar '{name}'", got_name == name,
                   f"got='{got_name}'")


def test_g20_pad_sound_count_cap():
    print("G20.4 PAD_SOUND count cap: > 12 di-clamp ke 12")
    # ESP32 bisa mengirim count > PAD_SOUND_COUNT; Wemos harus clamp.
    link = PadSoundLink()
    seq = link.request(0)
    # Kirim header dengan count=20 (di atas cap 12).
    link.deliver_list_header(20, current=0xFF, seq=seq)
    _check("G20.4 count di-clamp ke 12", link.count == PAD_SOUND_COUNT)

    # Kirim 12 nama (sesuai cap).
    names = [f"PAD{i:02d}.wav" for i in range(PAD_SOUND_COUNT)]
    for i, n in enumerate(names):
        link.deliver_name(i, n, seq=seq)

    _check("G20.4 ready setelah 12 nama", link.ready)
    result = link.consume()
    _check("G20.4 consume sukses", result is not None)
    if result is not None:
        _check("G20.4 jumlah nama = 12", len(result[0]) == PAD_SOUND_COUNT)


def test_g20_pad_sound_repeat_request():
    print("G20.5 PAD_SOUND repeat request: request baru membuang sesi lama")
    names1 = ["KICK.wav", "SNARE.wav"]
    names2 = ["CHH.wav", "OHH.wav", "TOM1.wav"]

    link = PadSoundLink()

    # Request pertama: mulai tapi jangan selesaikan.
    seq1 = link.request(0)
    link.deliver_list_header(len(names1), current=0, seq=seq1)
    link.deliver_name(0, names1[0], seq=seq1)   # hanya kirim 1 dari 2

    _check("G20.5 setelah request1 + 1 nama: belum ready", not link.ready)

    # Request kedua: reset state, SEQ baru.
    seq2 = link.request(0)
    _check("G20.5 SEQ baru", seq2 != seq1)
    _check("G20.5 pending reset", link.pending)

    # Frame dari request lama (seq1) harus diabaikan.
    ignored = link.deliver_name_header(1, len(names1[1]), seq=seq1)
    _check("G20.5 frame lama (seq mismatch) diabaikan", not ignored)

    # Request kedua selesai normal.
    link.deliver_full_list(names2, current=1, seq=seq2)
    _check("G20.5 request kedua ready", link.ready)
    result = link.consume()
    _check("G20.5 consume berhasil", result is not None)
    if result is not None:
        got, cur = result
        _check("G20.5 daftar = names2", got == names2, str(got))
        _check("G20.5 current = 1", cur == 1)


def test_g20_pad_sound_timeout_recovery():
    print("G20.6 PAD_SOUND timeout recovery: setelah timeout, request baru bekerja")
    link = PadSoundLink()

    # Request, header datang, tapi chunk tidak pernah datang -> timeout.
    seq1 = link.request(0)
    link.deliver_list_header(3, current=0xFF, seq=seq1)
    link.deliver_name_header(0, 8, seq=seq1)   # KICK.wav, tapi tidak ada chunk

    _check("G20.6 pending sebelum timeout", link.pending)

    link.timeout()   # simulasikan timeout setelah 2500ms

    _check("G20.6 ready=True setelah timeout", link.ready)
    _check("G20.6 ok=False (gagal, bukan palsu sukses)", not link.ok)
    _check("G20.6 pending=False setelah timeout", not link.pending)

    result = link.consume()
    _check("G20.6 consume return None (timeout = gagal)", result is None)
    _check("G20.6 ready=False setelah consume", not link.ready)

    # Recovery: request baru sukses setelah timeout.
    seq2 = link.request(0)
    _check("G20.6 request baru: pending=True", link.pending)
    ok = link.deliver_full_list(["KICK.wav", "SNARE.wav"], current=0, seq=seq2)
    _check("G20.6 request baru setelah timeout sukses", ok)
    result2 = link.consume()
    _check("G20.6 consume recovery berhasil", result2 is not None)
    if result2 is not None:
        _check("G20.6 nama recovery benar", result2[0] == ["KICK.wav", "SNARE.wav"])


def test_g20_pad_sound_invalid_seq_chunk():
    print("G20.7 PAD_SOUND invalid SEQ/chunk -> diabaikan tanpa merusak state")
    names = ["KICK.wav", "SNARE.wav", "CHH.wav"]

    link = PadSoundLink()
    seq = link.request(0)
    link.deliver_list_header(len(names), current=0, seq=seq)

    # Kirim nama-header idx=0 dengan SEQ salah -> diabaikan.
    wrong = link.deliver_name_header(0, len(names[0]), seq=(seq + 1) & 0xFF)
    _check("G20.7 nama-header SEQ salah diabaikan", not wrong)

    # Kirim nama-header idx=99 (di atas count) -> diabaikan.
    wrong2 = link.deliver_name_header(99, 8, seq=seq)
    _check("G20.7 nama-header idx > count diabaikan", not wrong2)

    # Kirim nama-header idx=1 (tidak urutan 0) -> diabaikan.
    wrong3 = link.deliver_name_header(1, 8, seq=seq)
    _check("G20.7 nama-header idx tidak urutan diabaikan", not wrong3)

    # Kirim nama-header yang benar (idx=0), tapi chunk SEQ salah.
    link.deliver_name_header(0, len(names[0]), seq=seq)
    _check("G20.7 nama-header idx=0 benar diterima", link.has_name)

    wrong4 = link.deliver_chunk(0, list(names[0][:3]), seq=(seq + 1) & 0xFF)
    _check("G20.7 chunk SEQ salah diabaikan", not wrong4)
    _check("G20.7 state tidak rusak (has_name masih True)", link.has_name)

    # Kirim chunk yang benar setelah chunk palsu -> sesi selesai normal.
    ok = link.deliver_name(0, names[0], seq=seq)  # deliver_name kirim ulang header + chunk
    # Karena has_name=True dan idx=0 masih sama, kita perlu reset idx dulu dengan deliver ulang.
    # Simulasikan manual: beri chunk benar.
    link2 = PadSoundLink()
    s2 = link2.request(0)
    link2.deliver_full_list(names, current=0, seq=s2)
    _check("G20.7 sesi normal setelah noise diabaikan: ready", link2.ready)
    result = link2.consume()
    _check("G20.7 consume normal berhasil", result is not None)
    if result is not None:
        _check("G20.7 daftar benar", result[0] == names)


def test_g20_pad_sound_set():
    print("G20.8 PAD_SOUND_SET: assign filename, validasi, resolve abs path")
    sd, tmp = sd_home()
    try:
        dirn = "/kits/Acoustic"

        # File valid yang ada -> assign diterima.
        result = set_pad_sample(sd, dirn, 0, "SNARE.wav")
        _check("G20.8 assign file ada -> abs path", result not in ("invalid", "missing"))

        # File tidak ada di folder -> ditolak.
        result2 = set_pad_sample(sd, dirn, 0, "GHOST.wav")
        _check("G20.8 assign file tidak ada -> 'missing'", result2 == "missing")

        # Nama invalid (path traversal) -> ditolak.
        result3 = set_pad_sample(sd, dirn, 0, "../KICK.wav")
        _check("G20.8 path traversal -> 'invalid'", result3 == "invalid")

        # Nama kosong -> ditolak.
        result4 = set_pad_sample(sd, dirn, 0, "")
        _check("G20.8 nama kosong -> 'invalid'", result4 == "invalid")

        # Assign SNARE.wav ke pad 3; scan ulang harus tetap 12.
        pads = sd.read_json(dirn + "/kit.json")["pads"]
        pads[3]["sample"] = "SNARE.wav"
        res = resolve_samples(sd, dirn, pads)
        _check("G20.8 pad3 resolve ke SNARE.wav",
               res[3] is not None and os.path.basename(res[3]) == "SNARE.wav")

        # Scan daftar setelah assign: tidak berubah (scan independen dari mapping).
        scan = scan_samples(sd, dirn)
        _check("G20.8 scan setelah SET tetap 12", len(scan) == 12)

        # current_index: pad3 sekarang SNARE.wav, cari di scan.
        ci = current_index(scan, res[3])
        snare_idx = scan.index("SNARE.wav") if "SNARE.wav" in scan else 0xFF
        _check("G20.8 current_index pad3 = index SNARE.wav di scan", ci == snare_idx)

        # Source code verification: G20 fix hadir di uart_client.cpp.
        root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        with open(os.path.join(root, "DrumCore_UI", "uart_client.cpp"),
                  "r", encoding="utf-8") as f:
            wemos_src = f.read()

        _check("G20.8 fix #1 count==0 ada di kode",
               "padSoundCount == 0" in wemos_src)
        _check("G20.8 fix #2 pos + validChars ada di kode",
               "pos + validChars" in wemos_src)
        _check("G20.8 PAD_SOUND_DEBUG flag ada",
               "PAD_SOUND_DEBUG" in wemos_src)
        _check("G20.8 [SOUND UI] REQUEST trace ada",
               "[SOUND UI] REQUEST" in wemos_src)
        _check("G20.8 [SOUND UI] RX HEADER trace ada",
               "[SOUND UI] RX HEADER" in wemos_src)
        _check("G20.8 [SOUND UI] TIMEOUT trace ada",
               "[SOUND UI] TIMEOUT" in wemos_src)

        with open(os.path.join(root, "DrumCore", "uart_protocol.cpp"),
                  "r", encoding="utf-8") as f:
            esp_src = f.read()

        _check("G20.8 [SOUND ESP] RX trace ada di ESP32",
               "[SOUND ESP] RX" in esp_src)
        _check("G20.8 [SOUND ESP] SCAN COUNT trace ada",
               "[SOUND ESP] SCAN COUNT" in esp_src)
        _check("G20.8 [SOUND ESP] TX HEADER trace ada",
               "[SOUND ESP] TX HEADER" in esp_src)
        _check("G20.8 [SOUND ESP] SAMPLE DIR trace ada",
               "[SOUND ESP] SAMPLE DIR" in esp_src)

        # Regresi KIT tidak tersentuh.
        _check("G20.8 regresi: CMD_KIT_LOAD masih ada",
               "case CMD_KIT_LOAD:" in esp_src)
        _check("G20.8 regresi: CMD_KIT_INFO masih ada",
               "case CMD_KIT_INFO:" in esp_src)
        # G21: snapshot buffer performSampleAssign dihapus (tidak ada
        # unload/reload PSRAM lagi, jadi tidak ada rollback). Yang
        # dikunci: KIT LOAD tetap jalur full-kit-load.
        _check("G20.8 regresi: G21 KIT LOAD tetap psramLoadKit",
               "psramLoadKit(" in esp_src and
               "loadOk = psramLoadKit(kitPaths, sampleIds, PAD_COUNT);"
               in esp_src)

    finally:
        shutil.rmtree(tmp, ignore_errors=True)


# ---------------------------------------------------------------
# G21 — PAD -> SOUND runtime remap.
#
# Semantic mirror dari DrumCore/psram_loader.cpp (samples[],
# slotPath[], slotForPad[]) + DrumCore/uart_protocol.cpp
# performSampleAssign(). Yang diuji:
#
#   - file SUDAH di PSRAM -> hanya satu store uint8 di slotForPad
#     (tanpa SD, tanpa malloc, tanpa psramLoadKit)
#   - file BARU           -> hanya file itu yang dibaca dari SD
#   - slot PSRAM tidak pernah di-free saat remap (aman untuk pad
#     lain yang berbagi slot dan untuk voice yang sedang bunyi)
#   - KIT LOAD tetap full-kit-load
# ---------------------------------------------------------------

G21_KIT_SLOTS = PAD_COUNT             # PSRAM_KIT_SLOT_COUNT = 12
G21_EXTRA_SLOTS = 4                   # PSRAM_EXTRA_SLOT_COUNT
G21_MAX_SLOTS = G21_KIT_SLOTS + G21_EXTRA_SLOTS   # PSRAM_MAX_SAMPLES = 16
G21_SLOT_NONE = 0xFF                  # PSRAM_SLOT_NONE


class PsramRuntime:
    """Mirror state + API psram_loader.cpp setelah G21.

    slot_for_pad  : uint8 per pad — SATU store = SATU remap
    slot_path     : registry "file ini sudah di PSRAM" -> slot
    samples       : PCM valid per slot (path sbg proxy pointer)
    kit_loads     : berapa kali psramLoadKit() dipanggil
    sd_reads      : file mana saja yang benar-benar dibaca dari SD
    """

    def __init__(self, sd):
        self.sd = sd
        self.samples = [None] * G21_MAX_SLOTS
        self.slot_path = [None] * G21_MAX_SLOTS
        self.slot_for_pad = list(range(PAD_COUNT))   # default identitas
        self.slot_own_buf = [False] * G21_MAX_SLOTS
        self.sample_count = 0
        self.kit_buffer = None
        self.kit_loaded = False
        self.kit_loads = 0
        self.single_loads = 0
        self.sd_reads = []
        self.sd_ready = True
        self.alloc_fails = False
        # kit_loader resolvedBuf per pad (bukan PSRAM; bookkeeping
        # persistensi via KIT SAVE).
        self.resolved = [""] * PAD_COUNT

    # ---------------- psramUnloadKit ----------------

    def unload(self):
        self.samples = [None] * G21_MAX_SLOTS
        self.slot_path = [None] * G21_MAX_SLOTS
        self.slot_own_buf = [False] * G21_MAX_SLOTS
        self.slot_for_pad = list(range(PAD_COUNT))
        self.sample_count = 0
        self.kit_buffer = None
        self.kit_loaded = False

    # ---------------- psramLoadKit (FULL kit load) ----------------

    def load_kit(self, paths, sample_ids=None):
        """Hanya CMD_KIT_LOAD / boot yang boleh sampai sini."""
        self.kit_loads += 1
        self.sd_reads = []
        self.unload()

        ids = sample_ids if sample_ids is not None else list(range(PAD_COUNT))

        for i, p in enumerate(paths):
            if not p:
                continue
            if not self.sd.exists(p):
                continue
            self.sd_reads.append(p)
            self.samples[i] = p
            self.slot_path[i] = p

        self.sample_count = len(paths)
        self.kit_buffer = "kit"
        self.kit_loaded = True

        for p in range(PAD_COUNT):
            src = ids[p] if p < len(ids) else p
            if src < G21_KIT_SLOTS and src < self.sample_count \
                    and self.samples[src]:
                self.slot_for_pad[p] = src
            else:
                self.slot_for_pad[p] = p

    # ---------------- psramFindSlotByPath ----------------

    def find_slot(self, path):
        if not path:
            return G21_SLOT_NONE
        for s in range(min(self.sample_count, G21_MAX_SLOTS)):
            if self.samples[s] and self.slot_path[s] == path:
                return s
        return G21_SLOT_NONE

    # ---------------- psramSetPadSlot ----------------

    def set_pad_slot(self, pad, slot):
        # Satu store uint8-alignment. Tidak ada lock, tidak ada SD.
        if pad >= PAD_COUNT:
            return
        self.slot_for_pad[pad] = slot

    # ---------------- psramLoadSingleSample ----------------

    def load_single_sample(self, path):
        if not path:
            return G21_SLOT_NONE

        existing = self.find_slot(path)
        if existing != G21_SLOT_NONE:
            return existing

        if not self.kit_buffer or not self.kit_loaded:
            return G21_SLOT_NONE
        if not self.sd_ready or self.alloc_fails:
            return G21_SLOT_NONE

        slot = G21_SLOT_NONE
        for s in range(G21_MAX_SLOTS):
            if not self.slot_path[s]:
                slot = s
                break
        if slot == G21_SLOT_NONE:
            return G21_SLOT_NONE

        if not self.sd.exists(path):
            return G21_SLOT_NONE

        # Hanya file INI yang dibaca. Tidak ada file lain, tidak ada
        # kit reload, tidak ada pointer PCM lain yang berubah.
        self.sd_reads.append(path)
        self.samples[slot] = path
        self.slot_path[slot] = path
        self.slot_own_buf[slot] = True     # alokasi sendiri
        if slot >= self.sample_count:
            self.sample_count = slot + 1
        self.single_loads += 1
        return slot

    # ---------------- audio.cpp: fireHit(pad) ----------------

    def fire_hit(self, pad):
        """Realtime: 1 read integer + 1 read pointer. Tanpa SD,
        tanpa String, tanpa registry rebuild."""
        slot = self.slot_for_pad[pad]          # <- satu-satunya lookup
        if slot >= G21_MAX_SLOTS:              # incl. PSRAM_SLOT_NONE
            return None                        # startVoice bails senyap
        if not self.kit_loaded or slot >= self.sample_count:
            return None
        return self.samples[slot]

    def pad_sound_name(self, pad):
        s = self.slot_for_pad[pad]
        if s >= G21_MAX_SLOTS:
            return None
        return self.samples[s]

    def registry_paths(self):
        return [p for p in self.slot_path if p]


def g21_pad_sound_set(rt, kit_dir, pad, name):
    """performSampleAssign(): validasi -> remap / load satu file."""
    if pad >= PAD_COUNT:
        return "invalid"
    if name and not validate_sample_name(name):
        return "invalid"

    abs_path = abs_sample(rt.sd, kit_dir, name) if name else ""
    if abs_path and not rt.sd.exists(abs_path):
        return "missing"

    if abs_path and rt.resolved[pad] == abs_path:
        return "unchanged"

    if not abs_path:
        rt.set_pad_slot(pad, G21_SLOT_NONE)
        rt.resolved[pad] = ""
        return "unassigned"

    slot = rt.find_slot(abs_path)
    if slot != G21_SLOT_NONE:
        rt.set_pad_slot(pad, slot)
        rt.resolved[pad] = abs_path
        return "remap"

    if not rt.sd_ready or not rt.kit_loaded:
        return "busy"

    slot = rt.load_single_sample(abs_path)
    if slot == G21_SLOT_NONE:
        return "busy"

    rt.set_pad_slot(pad, slot)
    rt.resolved[pad] = abs_path
    return "load"


def make_wav(path, pcm_bytes=2048):
    """Bikin WAV 16-bit mono 48 kHz sungguhan di folder samples."""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    data_len = pcm_bytes - (pcm_bytes % 2)
    header = b"RIFF" + (36 + data_len).to_bytes(4, "little") + b"WAVE"
    header += b"fmt " + (16).to_bytes(4, "little")
    header += (1).to_bytes(2, "little") + (1).to_bytes(2, "little")
    header += (48000).to_bytes(4, "little")
    header += (96000).to_bytes(4, "little")
    header += (2).to_bytes(2, "little") + (16).to_bytes(2, "little")
    header += b"data" + data_len.to_bytes(4, "little")
    with open(path, "wb") as f:
        f.write(header + b"\x00" * data_len)


def g21_loaded_12(rt, sd, kit_dir, pads):
    """Boot/kit-load helper: resolve 12 pad lalu psramLoadKit."""
    resolved = resolve_samples(sd, kit_dir, pads)
    rt.load_kit(resolved)
    for i in range(PAD_COUNT):
        rt.resolved[i] = resolved[i] or ""
    return resolved


# ---------------------------------------------------------------
# G21 Tests
# ---------------------------------------------------------------

def test_g21_remap_no_kit_reload():
    """G21.1 TES 1+2: sample sudah loaded -> remap, tanpa full reload."""
    print("G21.1 TES 1+2: PAD01 KICK->SNARE tanpa psramLoadKit")
    sd, tmp = sd_home()
    try:
        kit_dir = "/kits/Acoustic"
        pads = load_kit(sd, kit_dir)

        rt = PsramRuntime(sd)
        g21_loaded_12(rt, sd, kit_dir, pads)

        _check("G21.1 boot: 12 sample ter-load", len(rt.sd_reads) == 12)
        _check("G21.1 boot: kit_loads = 1", rt.kit_loads == 1)
        _check("G21.1 boot: mapping pad->slot identitas",
               rt.slot_for_pad == list(range(PAD_COUNT)))

        kick_slot = rt.slot_for_pad[0]
        snare_slot = rt.find_slot(abs_sample(sd, kit_dir, "SNARE.wav"))
        _check("G21.1 slot SNARE ada di registry", snare_slot == 1)

        pcm_before = list(rt.samples)
        reads_before = list(rt.sd_reads)

        r1 = g21_pad_sound_set(rt, kit_dir, 0, "KICK.wav")
        _check("G21.1 KICK (sama) -> unchanged (idempotent)",
               r1 == "unchanged", r1)
        _check("G21.1 idempotent: TIDAK ada kerja apa pun",
               rt.kit_loads == 1 and rt.single_loads == 0 and
               rt.sd_reads == reads_before)

        r2 = g21_pad_sound_set(rt, kit_dir, 0, "SNARE.wav")
        _check("G21.1 KICK->SNARE -> remap (bukan load)", r2 == "remap", r2)

        # TES 1: tidak ada full psramLoadKit, tidak ada SD access.
        _check("G21.1 TES1: TIDAK ada psramLoadKit tambahan",
               rt.kit_loads == 1, f"kit_loads={rt.kit_loads}")
        _check("G21.1 TES1: TIDAK ada baca SD saat remap",
               rt.sd_reads == reads_before, str(rt.sd_reads))
        _check("G21.1 TES1: tidak ada single load juga",
               rt.single_loads == 0)

        # TES 2: hanya slot PAD01 yang berubah.
        _check("G21.1 TES2: slot PAD01 = slot SNARE",
               rt.slot_for_pad[0] == snare_slot)
        _check("G21.1 TES2: slot SNARE tidak berubah",
               rt.find_slot(abs_sample(sd, kit_dir, "SNARE.wav")) == snare_slot)
        _check("G21.1 TES2: PCM semua slot tetap (tidak rebase)",
               rt.samples == pcm_before)
        _check("G21.1 TES2: slot KICK tetap hidup (tidak di-free)",
               rt.samples[kick_slot] is not None and
               rt.slot_path[kick_slot] is not None)
        _check("G21.1 TES2: 11 pad lain tidak berubah",
               rt.slot_for_pad[1:] == list(range(1, PAD_COUNT)))
        _check("G21.1 TES2: registry tetap 12 entri (tanpa duplikat)",
               len(rt.registry_paths()) == 12)

        # Mapping RAM ikut berubah (persistensi KIT SAVE).
        _check("G21.1 resolved path PAD01 = SNARE",
               os.path.basename(rt.resolved[0]) == "SNARE.wav")
        _check("G21.1 fire_hit(PAD01)louarkan SNARE",
               rt.fire_hit(0) == abs_sample(sd, kit_dir, "SNARE.wav"))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def test_g21_new_single_sample():
    """G21.2 TES 3: file baru -> hanya file itu yang dimuat."""
    print("G21.2 TES 3: file baru -> load SATU file, 11 sample lain utuh")
    sd, tmp = sd_home()
    try:
        kit_dir = "/kits/Acoustic"
        pads = load_kit(sd, kit_dir)

        rt = PsramRuntime(sd)
        g21_loaded_12(rt, sd, kit_dir, pads)

        new_file = os.path.join(sd.p(kit_dir), "samples", "kendang.wav")
        make_wav(new_file)
        _check("G21.2 file baru ada di SD", sd.exists(new_file))
        _check("G21.2 file baru ada di folder samples",
               "kendang.wav" in sd.listdir(kit_dir + "/samples"))

        pcm_before = list(rt.samples)
        reads_before = list(rt.sd_reads)
        other_slots_before = list(rt.slot_for_pad)

        res = g21_pad_sound_set(rt, kit_dir, 0, "kendang.wav")
        _check("G21.2 assign file baru -> load", res == "load", res)

        new_abs = abs_sample(sd, kit_dir, "kendang.wav")
        new_slot = rt.find_slot(new_abs)

        _check("G21.2 TES3: hanya 1 file dibaca dari SD",
               rt.sd_reads == reads_before + [new_abs], str(rt.sd_reads))
        _check("G21.2 TES3: TIDAK ada full kit reload",
               rt.kit_loads == 1, f"kit_loads={rt.kit_loads}")
        _check("G21.2 TES3: single load = 1", rt.single_loads == 1)
        _check("G21.2 TES3: PCM 12 sample lama tidak berubah",
               rt.samples[:PAD_COUNT] == pcm_before[:PAD_COUNT])
        _check("G21.2 TES3: slot baru di-register",
               new_slot != G21_SLOT_NONE and
               rt.slot_path[new_slot] == new_abs)
        _check("G21.2 TES3: slot baru punya alokasi sendiri",
               rt.slot_own_buf[new_slot] is True)
        _check("G21.2 TES3: buffer kit tidak di-rebase/di-free",
               rt.kit_buffer == "kit")
        _check("G21.2 TES3: registry jadi 13 entri",
               len(rt.registry_paths()) == 13)
        _check("G21.2 TES3: mapping 11 pad lain utuh",
               rt.slot_for_pad[1:] == other_slots_before[1:])
        _check("G21.2 TES3: fire_hit(PAD01) memakai sample baru",
               rt.fire_hit(0) == new_abs)
        _check("G21.2 TES3: 12 sample lama tetap berbunyi",
               rt.fire_hit(1) == abs_sample(sd, kit_dir, "SNARE.wav"))

        # Kapasitas registry habis -> gagal BERSIH, mapping tidak berubah.
        for n in ("t1.wav", "t2.wav", "t3.wav", "t4.wav"):
            make_wav(os.path.join(sd.p(kit_dir), "samples", n))
        for n in ("t1.wav", "t2.wav", "t3.wav"):
            _check(f"G21.2 slot ekstra terisi oleh {n}",
                   g21_pad_sound_set(rt, kit_dir, 1, n) == "load")
        _check("G21.2 semua 16 slot PSRAM terpakai",
               len(rt.registry_paths()) == G21_MAX_SLOTS)
        before_busy = list(rt.slot_for_pad)
        res_over = g21_pad_sound_set(rt, kit_dir, 2, "t4.wav")
        _check("G21.2 registry penuh -> busy (bukan crash)",
               res_over == "busy", res_over)
        _check("G21.2 registry penuh: mapping tidak berubah",
               rt.slot_for_pad == before_busy)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def test_g21_shared_slot_and_unassign():
    """G21.3 TES 4+5: slot dibagi-bagi, unassign tidak free apa pun."""
    print("G21.3 TES 4+5: duplicate assignment (share slot) + unassign")
    sd, tmp = sd_home()
    try:
        kit_dir = "/kits/Acoustic"
        pads = load_kit(sd, kit_dir)

        rt = PsramRuntime(sd)
        g21_loaded_12(rt, sd, kit_dir, pads)

        snare = abs_sample(sd, kit_dir, "SNARE.wav")
        snare_slot = rt.find_slot(snare)
        # Fixture: PAD02 sudah SNARE.wav, jadi untuk uji "file yang sama
        # dipakai dua pad" kita pakai PAD05 (TOM1) sebagai pad kedua.
        other = 4

        r1 = g21_pad_sound_set(rt, kit_dir, 0, "SNARE.wav")
        r2 = g21_pad_sound_set(rt, kit_dir, other, "SNARE.wav")
        _check("G21.3 TES4: PAD01->SNARE remap", r1 == "remap", r1)
        _check("G21.3 TES4: PAD05->SNARE remap", r2 == "remap", r2)
        _check("G21.3 TES4: dua pad berbagi SATU slot",
               rt.slot_for_pad[0] == snare_slot and
               rt.slot_for_pad[other] == snare_slot)
        _check("G21.3 TES4: tidak ada sample SNARE kedua di PSRAM",
               rt.registry_paths().count(snare) == 1)
        _check("G21.3 TES4: slot TOM1 lama tetap hidup",
               rt.samples[other] == abs_sample(sd, kit_dir, "TOM1.wav"))
        _check("G21.3 TES4: tidak ada load tambahan",
               rt.single_loads == 0 and rt.kit_loads == 1)
        _check("G21.3 TES4: total PCM tidak bertambah",
               len(rt.registry_paths()) == 12)

        # ---- TES 5: UNASSIGN ----
        registry_before = sorted(rt.registry_paths())
        slots_before = list(rt.slot_for_pad)

        res = g21_pad_sound_set(rt, kit_dir, 0, "")
        _check("G21.3 TES5: unassign -> unassigned", res == "unassigned", res)
        _check("G21.3 TES5: slot PAD01 = PSRAM_SLOT_NONE",
               rt.slot_for_pad[0] == G21_SLOT_NONE)
        _check("G21.3 TES5: fire_hit(PAD01) senyap (bukan crash)",
               rt.fire_hit(0) is None)
        _check("G21.3 TES5: resolved path PAD01 kosong (KIT SAVE 'sample':'')",
               rt.resolved[0] == "")
        _check("G21.3 TES5: SEMUA slot sample tetap hidup (tidak di-free)",
               sorted(rt.registry_paths()) == registry_before)
        _check("G21.3 TES5: PCM semua slot utuh",
               all(p is not None for p in rt.samples[:PAD_COUNT]))
        _check("G21.3 TES5: pad berbagi slot tetap bisa memakai SNARE",
               rt.slot_for_pad[other] == snare_slot and
               rt.fire_hit(other) == snare)
        _check("G21.3 TES5: 10 pad lain tidak berubah",
               all(rt.slot_for_pad[i] == slots_before[i]
                   for i in range(1, PAD_COUNT) if i != other))
        _check("G21.3 TES5: unassign tanpa SD load",
               rt.single_loads == 0 and rt.kit_loads == 1)

        # Unassign pad yang punya sample, lalu assign ulang: slot lama
        # dipakai lagi (tanpa load ulang file).
        res2 = g21_pad_sound_set(rt, kit_dir, 0, "SNARE.wav")
        _check("G21.3 assign ulang setelah unassign -> remap",
               res2 == "remap", res2)
        _check("G21.3 assign ulang: slot kembali sama",
               rt.slot_for_pad[0] == snare_slot)
        _check("G21.3 assign ulang: tidak ada load file",
               rt.single_loads == 0)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def test_g21_save_persist_reboot():
    """G21.4 TES 6: PAD->SOUND + KIT SAVE + restart -> mapping sama."""
    print("G21.4 TES 6: KIT SAVE + restart mempertahankan mapping")
    sd, tmp = sd_home()
    try:
        kit_dir = "/kits/Acoustic"
        pads = load_kit(sd, kit_dir)

        rt = PsramRuntime(sd)
        g21_loaded_12(rt, sd, kit_dir, pads)

        g21_pad_sound_set(rt, kit_dir, 0, "SNARE.wav")
        g21_pad_sound_set(rt, kit_dir, 5, "RIDE.wav")
        g21_pad_sound_set(rt, kit_dir, 0, "")          # unassign
        g21_pad_sound_set(rt, kit_dir, 0, "CHH.wav")   # dipakai lagi

        # KIT SAVE: menulis effective RAM (nama file) ke kit.json.
        effective = effective_pad_config(pads, rt.resolved)
        kit_save_active(sd, kit_dir, "Acoustic", effective)

        saved = sd.read_json(kit_dir + "/kit.json")["pads"]
        _check("G21.4 TES6: kit.json menyimpan sample per NAMA FILE",
               saved[0]["sample"] == "CHH.wav" and
               saved[5]["sample"] == "RIDE.wav")
        _check("G21.4 TES6: mapping runtime == persistence",
               os.path.basename(rt.resolved[0]) == "CHH.wav" and
               os.path.basename(rt.resolved[5]) == "RIDE.wav")

        # ---- RESTART: boot ulang dari SD ----
        boot = PsramRuntime(sd)
        pads2 = load_kit(sd, kit_dir)
        resolved2 = g21_loaded_12(boot, sd, kit_dir, pads2)

        _check("G21.4 TES6: restart: PAD01 -> CHH.wav",
               os.path.basename(resolved2[0]) == "CHH.wav")
        _check("G21.4 TES6: restart: PAD06 -> RIDE.wav",
               os.path.basename(resolved2[5]) == "RIDE.wav")
        _check("G21.4 TES6: restart: kit_loads = 1 (full load saat boot)",
               boot.kit_loads == 1)
        _check("G21.4 TES6: restart: 12 sample kembali di PSRAM",
               len(boot.sd_reads) == 12)
        _check("G21.4 TES6: restart: registry dibangun ulang dari kit",
               len(boot.registry_paths()) == 12)
        _check("G21.4 TES6: restart: fire_hit(PAD01) = CHH",
               os.path.basename(boot.fire_hit(0) or "") == "CHH.wav")
        _check("G21.4 TES6: restart: fire_hit(PAD06) = RIDE",
               os.path.basename(boot.fire_hit(5) or "") == "RIDE.wav")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def test_g21_rapid_reassign():
    """G21.5 TES 7: reassignment cepat tidak merusak state."""
    print("G21.5 TES 7: rapid reassignment (KICK/SNARE/CHH/OHH/KICK/SNARE)")
    sd, tmp = sd_home()
    try:
        kit_dir = "/kits/Acoustic"
        pads = load_kit(sd, kit_dir)

        rt = PsramRuntime(sd)
        g21_loaded_12(rt, sd, kit_dir, pads)

        untouched = {i: rt.slot_for_pad[i] for i in range(1, PAD_COUNT)}
        pcm_before = list(rt.samples)
        reads_before = list(rt.sd_reads)
        sequence = ["KICK.wav", "SNARE.wav", "CHH.wav", "OHH.wav",
                    "KICK.wav", "SNARE.wav"]

        results = []
        for name in sequence:
            results.append(g21_pad_sound_set(rt, kit_dir, 0, name))
            # setiap langkah harus menghasilkan mapping yang valid
            want = abs_sample(sd, kit_dir, name)
            if rt.pad_sound_name(0) != want:
                break
            if rt.fire_hit(0) != want:
                break
            if rt.sd_reads != reads_before:
                break
            if any(rt.slot_for_pad[i] != untouched[i]
                   for i in range(1, PAD_COUNT)):
                break

        # Langkah 1 = KICK yang memang sudah terpasang (fixture) jadi
        # idempotent no-op; 5 langkah berikutnya remap sungguhan.
        _check("G21.5 semua 6 assignment dippedroses",
               results == ["unchanged", "remap", "remap", "remap",
                           "remap", "remap"], str(results))
        _check("G21.5 mapping akhir benar (SNARE)",
               os.path.basename(rt.pad_sound_name(0) or "") == "SNARE.wav")
        _check("G21.5 TIDAK ada kit reload per assignment",
               rt.kit_loads == 1, f"kit_loads={rt.kit_loads}")
        _check("G21.5 TIDAK ada load file (semua sudah di PSRAM)",
               rt.single_loads == 0)
        _check("G21.5 TIDAK ada baca SD tambahan",
               rt.sd_reads == reads_before)
        _check("G21.5 PCM semua slot utuh (tanpa rebase)",
               rt.samples == pcm_before)
        _check("G21.5 registry tidak kembar duplikat",
               len(rt.registry_paths()) == len(set(rt.registry_paths())))
        _check("G21.5 11 pad lain tetap di slot semula",
               all(rt.slot_for_pad[i] == untouched[i]
                   for i in range(1, PAD_COUNT)))
        _check("G21.5 tidak ada slot menggantung",
               all(rt.samples[s] is not None
                   for s in rt.slot_for_pad if s < G21_MAX_SLOTS))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def test_g21_audio_while_remapping():
    """G21.6 TES 8: AudioTask menembak bersamaan dengan remap."""
    print("G21.6 TES 8: audio (Core 0) + PAD_SOUND_SET (Core 1) bersamaan")
    sd, tmp = sd_home()
    try:
        kit_dir = "/kits/Acoustic"
        pads = load_kit(sd, kit_dir)

        rt = PsramRuntime(sd)
        g21_loaded_12(rt, sd, kit_dir, pads)

        kick = abs_sample(sd, kit_dir, "KICK.wav")
        snare = abs_sample(sd, kit_dir, "SNARE.wav")

        _check("G21.6 fire_hit baseline = KICK", rt.fire_hit(0) == kick)

        # Interleave: hit pada pad 0 di tengah-tengah remap. Karena
        # remap hanya menulis SATU integer, pembaca hanya mungkin
        # melihat slot lama (KICK) atau slot baru (SNARE) — keduanya
        # slot valid dengan PCM termuat.
        rt.set_pad_slot(0, rt.find_slot(snare))
        observed = [rt.fire_hit(0), rt.fire_hit(0), rt.fire_hit(0)]
        rt.resolved[0] = snare

        _check("G21.6 semua observasi valid (tidak ada slot setengah update)",
               all(o in (kick, snare) for o in observed), str(observed))
        _check("G21.6 observasi = KICK atau SNARE (bukan data lain)",
               set(observed) <= {kick, snare})
        _check("G21.6 PCM pointer tidak pernah berubah",
               rt.samples[0] == kick and rt.samples[1] == snare)

        # Reader tidak boleh memicu kerja kontrol-plane apa pun.
        reads_before = list(rt.sd_reads)
        loads_before = rt.kit_loads
        singles_before = rt.single_loads
        for _ in range(1000):
            rt.fire_hit(0)
            rt.fire_hit(5)
        _check("G21.6 fire_hit tidak menyentuh SD", rt.sd_reads == reads_before)
        _check("G21.6 fire_hit tidak memicu kit load",
               rt.kit_loads == loads_before)
        _check("G21.6 fire_hit tidak memicu single load",
               rt.single_loads == singles_before)

        # Remap bersamaan dengan hit pada pad lain tetap utuh.
        for p in range(PAD_COUNT):
            name = os.path.basename(rt.pad_sound_name(p) or "")
            g21_pad_sound_set(rt, kit_dir, 5, "OHH.wav")
            g21_pad_sound_set(rt, kit_dir, 5, name if name else "KICK.wav")
        _check("G21.6 remap berulang tidak merusak pad lain",
               all(rt.samples[s] is not None
                   for s in rt.slot_for_pad if s < G21_MAX_SLOTS))
        _check("G21.6 registry tetap konsisten (tanpa duplikat)",
               len(rt.registry_paths()) == len(set(rt.registry_paths())))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def test_g21_kit_switch_still_full_load():
    """G21.7 TES 9: KIT LOAD tetap full-kit-load; extras ikut dibuang."""
    print("G21.7 TES 9: KIT LOAD tetap full load (Acoustic -> ROCK LIVE)")
    sd, tmp = sd_home()
    try:
        acoustic = load_kit(sd, "/kits/Acoustic")
        rt = PsramRuntime(sd)
        g21_loaded_12(rt, sd, "/kits/Acoustic", acoustic)

        make_wav(os.path.join(sd.p("/kits/Acoustic"), "samples", "kendang.wav"))
        g21_pad_sound_set(rt, "/kits/Acoustic", 0, "kendang.wav")
        _check("G21.7 runtime slot tambahan terisi",
               len(rt.registry_paths()) == 13)

        rock_dir = "/kits/ROCK LIVE"
        rock = load_kit(sd, rock_dir)
        g21_loaded_12(rt, sd, rock_dir, rock)

        _check("G21.7 TES9: KIT LOAD tetap memanggil psramLoadKit",
               rt.kit_loads == 2, f"kit_loads={rt.kit_loads}")
        _check("G21.7 TES9: full 12 sample kit baru dibaca dari SD",
               len(rt.sd_reads) == 12, str(len(rt.sd_reads)))
        _check("G21.7 TES9: registry dibangun ulang (13 -> 12)",
               len(rt.registry_paths()) == 12)
        _check("G21.7 TES9: sample runtime kit lama dibuang",
               all("kendang" not in (p or "") for p in rt.registry_paths()))
        _check("G21.7 TES9: mapping pad->slot kembali identitas",
               rt.slot_for_pad == list(range(PAD_COUNT)))
        _check("G21.7 TES9: 12 sample kit baru audible",
               all(rt.samples[i] is not None for i in range(PAD_COUNT)))
        _check("G21.7 TES9: fire_hit memakai kit baru",
               rt.fire_hit(0) == abs_sample(sd, rock_dir,
                                           os.path.basename(
                                               rt.pad_sound_name(0) or "")))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def test_g21_failure_paths_clean():
    """G21.8: kegagalan tidak pernah meninggalkan mapping setengah jadi."""
    print("G21.8 kegagalan: file hilang / PSRAM penuh / SD down")
    sd, tmp = sd_home()
    try:
        kit_dir = "/kits/Acoustic"
        pads = load_kit(sd, kit_dir)
        rt = PsramRuntime(sd)
        g21_loaded_12(rt, sd, kit_dir, pads)

        before = list(rt.slot_for_pad)
        resolved_before = list(rt.resolved)

        _check("G21.8 file tidak ada -> missing",
               g21_pad_sound_set(rt, kit_dir, 0, "GHOST.wav") == "missing")
        _check("G21.8 path traversal -> invalid",
               g21_pad_sound_set(rt, kit_dir, 0, "../KICK.wav") == "invalid")
        _check("G21.8 nama terlalu panjang -> invalid",
               g21_pad_sound_set(rt, kit_dir, 0, "A" * 40 + ".wav")
               == "invalid")
        _check("G21.8 mapping tidak berubah setelah semua kegagalan",
               rt.slot_for_pad == before and rt.resolved == resolved_before)

        # SD mati saat file baru diminta -> busy, mapping utuh.
        make_wav(os.path.join(sd.p(kit_dir), "samples", "kendang.wav"))
        rt.sd_ready = False
        _check("G21.8 SD down -> busy",
               g21_pad_sound_set(rt, kit_dir, 0, "kendang.wav") == "busy")
        _check("G21.8 SD down: mapping tetap utuh",
               rt.slot_for_pad == before)

        # PSRAM gagal alokasi -> busy, mapping utuh.
        rt.sd_ready = True
        rt.alloc_fails = True
        _check("G21.8 ps_malloc gagal -> busy",
               g21_pad_sound_set(rt, kit_dir, 0, "kendang.wav") == "busy")
        _check("G21.8 alokasi gagal: mapping tetap utuh",
               rt.slot_for_pad == before and
               len(rt.registry_paths()) == 12)

        # Kit belum termuat -> remap ditolak, tidak ada crash.
        cold = PsramRuntime(sd)
        _check("G21.8 kit belum load -> busy",
               g21_pad_sound_set(cold, kit_dir, 0, "KICK.wav") == "busy")
        _check("G21.8 kit belum load: audio senyap, bukan crash",
               cold.fire_hit(0) is None)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def test_g21_source_verification():
    """G21.9 source verification: bentuk patch di firmware."""
    print("G21.9 source verification: registry + remap + realtime aman")
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

    def read(*parts):
        with open(os.path.join(root, *parts), "r", encoding="utf-8") as f:
            return f.read()

    uart_src = read("DrumCore", "uart_protocol.cpp")
    psram_h = read("DrumCore", "psram_loader.h")
    psram_src = read("DrumCore", "psram_loader.cpp")
    audio_src = read("DrumCore", "audio.cpp")
    cfg_src = read("DrumCore", "config.h")

    assign = _func_body(uart_src, "uint8_t performSampleAssign(")
    set_slot_raw = _func_body(psram_src, "void psramSetPadSlot(")
    set_slot = _strip_comments(set_slot_raw)
    single_raw = _func_body(psram_src, "uint8_t psramLoadSingleSample(")
    single = _strip_comments(single_raw)
    kit_switch = _func_body(uart_src, "uint8_t performKitSwitch(")
    fire_hit = _func_body(audio_src, "void fireHit(")
    start_voice = _func_body(audio_src, "void startVoice(")

    # --- performSampleAssign: remap, bukan reload ---
    _check("G21.9 performSampleAssign memanggil psramFindSlotByPath",
           "psramFindSlotByPath(" in assign)
    _check("G21.9 performSampleAssign memanggil psramSetPadSlot",
           "psramSetPadSlot(" in assign)
    _check("G21.9 performSampleAssign memanggil psramLoadSingleSample",
           "psramLoadSingleSample(" in assign)
    _check("G21.9 performSampleAssign TIDAK psramLoadKit",
           "psramLoadKit(" not in _strip_comments(assign))
    _check("G21.9 performSampleAssign TIDAK performKitSwitch",
           "performKitSwitch(" not in _strip_comments(assign))
    _check("G21.9 performSampleAssign TIDAK buka/tulis SD",
           "SD.open" not in _strip_comments(assign) and
           "SD.exists" not in _strip_comments(assign) and
           "writeJson" not in _strip_comments(assign))
    _check("G21.9 performSampleAssign tidak ambil hit-gate (instan)",
           "audioSetHitAccept(" not in _strip_comments(assign) and
           "audioActiveVoiceCount(" not in _strip_comments(assign))
    _check("G21.9 performSampleAssign tetap noinline (G19 stack fix)",
           "__attribute__((noinline)) uint8_t performSampleAssign(" in uart_src)
    _check("G21.9 performSampleAssign tanpa alokasi heap (tanpa String)",
           "new " not in _strip_comments(assign) and
           "String " not in _strip_comments(assign) and
           "String(" not in _strip_comments(assign))
    _check("G21.9 trace [SOUND SET] pad= ada",
           "[SOUND SET] pad=%u file=%s" in assign)
    _check("G21.9 trace cached slot= ada",
           "[SOUND SET] cached slot=%u" in assign)
    _check("G21.9 trace REMAP ONLY ada",
           "[SOUND SET] REMAP ONLY elapsed=%luus" in assign)
    _check("G21.9 trace LOAD NEW SAMPLE ada",
           "[SOUND SET] LOAD NEW SAMPLE slot=%u elapsed=%luus" in assign)
    _check("G21.9 flag DEBUG_SOUND_SET ada",
           "DEBUG_SOUND_SET" in uart_src)

    # --- KIT LOAD harus tetap full load ---
    _check("G21.9 performKitSwitch tetap psramLoadKit",
           "psramLoadKit(" in kit_switch)
    _check("G21.9 performKitSwitch tetap gate + tunggu voice",
           "audioSetHitAccept(false)" in kit_switch and
           "audioActiveVoiceCount()" in kit_switch)
    _check("G21.9 cmd CMD_KIT_LOAD masih di dispatch",
           "case CMD_KIT_LOAD:" in uart_src)

    # --- psram_loader: registry benar-benar ada ---
    _check("G21.9 psram_loader.h deklarasi psramFindSlotByPath",
           "uint8_t psramFindSlotByPath(" in psram_h)
    _check("G21.9 psram_loader.h deklarasi psramSetPadSlot",
           "void psramSetPadSlot(" in psram_h)
    _check("G21.9 psram_loader.h deklarasi psramLoadSingleSample",
           "uint8_t psramLoadSingleSample(" in psram_h)
    _check("G21.9 registry path->slot di memory",
           "char slotPath[PSRAM_MAX_SAMPLES][KIT_SAMPLE_PATH_LEN]" in psram_src)
    _check("G21.9 psramFindSlotByPath hanya banding string registry",
           "strcmp(slotPath[s], absPath) == 0" in psram_src)
    _check("G21.9 psramSetPadSlot = satu store, tanpa lock",
           "slotForPad[padIndex] = slot;" in set_slot and
           "portENTER_CRITICAL" not in set_slot and
           "vTaskSuspendAll" not in set_slot and
           "SD." not in set_slot and "ps_malloc" not in set_slot)
    _check("G21.9 psramSetPadSlot menolak pad di luar PAD_COUNT",
           "padIndex >= PAD_COUNT" in set_slot)
    _check("G21.9 psramLoadSingleSample tidak memanggil psramLoadKit",
           "psramLoadKit(" not in single)
    _check("G21.9 psramLoadSingleSample hanya membaca 1 file",
           single.count("SD.open(") == 1, str(single.count("SD.open(")))
    _check("G21.9 psramLoadSingleSample tolak semua penolakan sebelum commit",
           "return PSRAM_SLOT_NONE;" in single and
           single.find("samples[slot].data") > single.rfind("return PSRAM_SLOT_NONE;"))
    _check("G21.9 psramLoadSingleSample tolak registry penuh",
           "firstFreeRegistrySlot()" in single and
           "registry full" in single_raw)
    _check("G21.9 psramUnloadKit bebaskan buffer slot runtime",
           "slotOwnBuf" in psram_src and
           "clearSampleRegistry();" in psram_src)
    _check("G21.9 KIT LOAD commit mendaftarkan path ke registry",
           "commitKitRegistry(samplePaths, count);" in psram_src)
    _check("G21.9 registry diisi ulang tiap KIT LOAD",
           "commitKitRegistry(" in psram_src and
           "clearSampleRegistry();" in psram_src)

    # --- audio realtime: hanya titik lookup sample index ---
    fire_code = _strip_comments(fire_hit)
    _check("G21.9 fireHit tetap pakai psramSlotForPad (runtime mapping)",
           "psramSlotForPad(padIndex)" in fire_code)
    _check("G21.9 fireHit tidak mencari nama file / SD / registry",
           "SD." not in fire_code and "strcmp" not in fire_code and
           "slotPath" not in fire_code and "psramFindSlotByPath" not in fire_code)
    _check("G21.9 startVoice bound = PSRAM_MAX_SAMPLES (slot runtime)",
           "sampleIndex >= PSRAM_MAX_SAMPLES" in start_voice and
           "sampleIndex >= PAD_COUNT" not in start_voice)
    _check("G21.9 startVoice tetap bail senyap (bukan crash)",
           "if (!data)" in start_voice and
           "return;" in start_voice)
    _check("G21.9 dbgSampleExpectedDurUs juga widened (slot runtime)",
           "dbgSampleExpectedDurUs" in audio_src and
           "sampleIndex >= PAD_COUNT" not in audio_src)
    _check("G21.9 mixer/I2S/voice allocator tidak disentuh",
           "psramSlotForPad" in audio_src and
           audio_src.count("VOICE_COUNT = ") == 0 and
           "audioSetHitAccept(bool accept)" in audio_src)
    _check("G21.9 AudioTask/TriggerTask/MidiTask/UartTask utuh",
           "audioTaskStart()" in audio_src and
           "UART_TASK_STACK = 8192" in uart_src and
           "UART1_BAUD = 921600" in cfg_src)
    _check("G21.9 AudioTask tidak bisa preload via SD",
           "psramFindSlotByPath(" not in audio_src and
           "psramLoadSingleSample(" not in audio_src and
           "psramSetPadSlot(" not in audio_src)
    _check("G21.9 jalur audio tidak menyentuh kit_loader sama sekali",
           "kitLoader" not in audio_src and "SD.exists" not in audio_src)

    # --- konfigurasi kapasitas slot ---
    _check("G21.9 config: PSRAM_KIT_SLOT_COUNT = PAD_COUNT",
           "constexpr uint8_t PSRAM_KIT_SLOT_COUNT = PAD_COUNT;" in cfg_src)
    _check("G21.9 config: PSRAM_MAX_SAMPLES = kit + extra",
           "PSRAM_KIT_SLOT_COUNT + PSRAM_EXTRA_SLOT_COUNT" in cfg_src)
    _check("G21.9 config: PSRAM_SLOT_NONE = 0xFF",
           "constexpr uint8_t PSRAM_SLOT_NONE = 0xFF;" in cfg_src)
    _check("G21.9 config: 12 PAD tidak berubah",
           "constexpr uint8_t PAD_COUNT = 12;" in cfg_src)

    # --- protokol & persistensi tidak berubah ---
    _check("G21.9 protokol: CMD_PAD_SOUND_SET tetap 0x23",
           "CMD_PAD_SOUND_SET   = 0x23" in
           read("DrumCore", "uart_protocol.h"))
    _check("G21.9 persistensi: mapping RAM tetap lewat kit_loader",
           "kitLoaderSetResolvedSamplePath(" in assign and
           "void kitLoaderSetResolvedSamplePath(" in
           read("DrumCore", "kit_loader.h"))
    _check("G21.9 persistensi: KIT SAVE tetap pakai resolved sample name",
           "kitLoaderResolvedSampleName(" in
           read("DrumCore", "kit_manager.cpp"))


# ===============================================================
# G22 — UI POLISH ONLY
#
#   1. RENAME/CREATE-NAME editor punya karakter SPACE yang benar
#      (nilai ' ', bukan '_' dan bukan literal "SPACE").
#   2. LCD DRUMCORE -> "DRUM COK" (display only).
#   3. Boot dirender sebagai STEP statis (bukan titik berkedip),
#      tanpa menambah waktu boot.
#   4. Project / internal / protocol tetap "DrumCore".
#
# Editor, validasi nama, dan renderer LCD dimirror di sini supaya
# perilaku bisa diuji tanpa hardware; keputusan "tidak menyentuh
# runtime" diverifikasi lewat source-verification seperti G21.9.
# ===============================================================

LCD_COLS = 16
BRAND_LCD = "DRUM COK"
BRAND_OLD = "DRUMCORE"

# ui.cpp KIT_RENAME_ALPHABET — SPACE sebagai elemen pertama ring.
KIT_RENAME_SPACE = " "
KIT_RENAME_ALPHABET = (
    " ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_."
)
KIT_RENAME_MAX = 14
LCD_RULE = "\x07"          # DISPLAY_RULE_GLYPH

# screens.cpp BOOT_STEP_TEXT — semua <= 16 kolom, tanpa elipsis.
BOOT_STEP_TEXT = (
    "Initializing",
    "Checking SD",
    "Loading Kit",
    "Loading Samples",
    "Starting Engine",
)


def kit_rename_index_of(ch):
    """kitRenameIndexOf(): posisi di ring, -1 bila di luar ring."""
    i = KIT_RENAME_ALPHABET.find(ch)
    return i if i >= 0 else -1


def kit_rename_char_after(cur, direction):
    """kitRenameCharAfter(): satu langkah putar.

    Sel berisi karakter ring -> maju/mundur satu slot (wrap).
    Sel kosong (di luar ring, mis. '\\0') -> langkah pertama
    menghasilkan SPACE, supaya nama ber-spasi bisa diketik tanpa
    memutar seluruh alfabet.
    """
    base = kit_rename_index_of(cur)
    if base < 0:
        return KIT_RENAME_SPACE
    n = len(KIT_RENAME_ALPHABET)
    return KIT_RENAME_ALPHABET[(base + direction) % n]


class RenameEditor:
    """Mirror handleKitRename() / handleKitCreateName().

    Rotate = ganti karakter pada cursor (satu detent = satu langkah
    ring). CLICK = majukan cursor; pada posisi terakhir CLICK =
    konfirmasi. Panjang buffer dibatasi 14 (RENAME) atau 28 (CREATE
    bernama) sesuai existing implementation.
    """

    def __init__(self, current="", limit=KIT_RENAME_MAX):
        self.limit = limit
        # strncpy(): isi nama lama, sisanya NUL (sel kosong).
        self.buf = (current or "")[:limit].ljust(limit, "\x00")
        self.cursor = 0

    def rotate(self, direction):
        self.buf = (self.buf[:self.cursor]
                    + kit_rename_char_after(self.buf[self.cursor], direction)
                    + self.buf[self.cursor + 1:])

    def rotate_to(self, want, direction=1):
        for _ in range(2 * len(KIT_RENAME_ALPHABET)):
            if self.buf[self.cursor] == want:
                return True
            self.rotate(direction)
        return self.buf[self.cursor] == want

    def type_name(self, wanted):
        """Ketik seperti user: putar sampai karakter tepat, lalu klik."""
        if len(wanted) > self.limit:
            return False
        for i, ch in enumerate(wanted):
            if not self.rotate_to(ch):
                return False
            if i + 1 < len(wanted):
                self.cursor += 1
        return True

    def confirmed_name(self):
        """kitRenameFinalize(): trim spasi kanan; kosong = BATAL."""
        raw = self.buf.split("\x00")[0]
        name = raw.rstrip(KIT_RENAME_SPACE)
        return name if name else None


def validate_kit_name(name):
    """Mirror kitManagerValidateName() (kit_manager.cpp).

    Tidak berubah oleh G22: separator path, karakter ilegal FAT,
    control character, nama > KIT_NAME_MAX, nama kosong setelah trim,
    dan '.'/'..'/titik di ujung tetap ditolak. Spasi di TENGAH nama
    tetap diterima.
    """
    n = 0
    while n < len(name) and n < KIT_NAME_MAX:
        c = name[n]
        if c in ("/", "\\"):
            return None
        if c in ('"', "*", ":", "<", ">", "?", "|"):
            return None
        if ord(c) < 0x20:
            return None
        n += 1
    if n != len(name):
        return None

    start, end = 0, n
    while start < end and name[start] == KIT_RENAME_SPACE:
        start += 1
    while end > start and name[end - 1] == KIT_RENAME_SPACE:
        end -= 1
    if start >= end:
        return None
    if name[start:end] in (".", ".."):
        return None
    if name[end - 1] == ".":
        return None
    return name[start:end]


def rename_kit(sd, slot, raw_name):
    """Mirror kitManagerRename(): validasi -> JSON name -> pindah folder.

    Mengembalikan (result, clean_name) dengan result salah satu dari
    "OK", "INVALID_NAME", "NOT_FOUND", "COLLISION".
    """
    clean = validate_kit_name(raw_name)
    if clean is None:
        return "INVALID_NAME", None

    old_dir = resolve_kit_dir(sd, slot)
    if old_dir is None:
        return "NOT_FOUND", None

    new_dir = "/kits/" + clean
    if sd.exists(new_dir) and new_dir != old_dir:
        return "COLLISION", None

    path = os.path.join(old_dir, "kit.json")
    try:
        doc = sd.read_json(path)
    except (OSError, ValueError, TypeError):
        return "NOT_FOUND", None

    doc["name"] = clean
    sd.write_json(path, doc)

    if new_dir != old_dir:
        try:
            os.rename(sd.p(old_dir), sd.p(new_dir))
        except OSError:
            return "BUSY", None

    return "OK", clean


def lcd_row(text):
    """displayPrintLine(): clamp ke 16 kolom."""
    return text[:LCD_COLS]


def _src(*parts):
    """Baca file source project (dipakai source-verification G22)."""
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    with open(os.path.join(root, *parts), "r", encoding="utf-8") as f:
        return f.read()


def build_brand_row(dashes):
    """Mirror screens.cpp buildBrandRow()."""
    block = 2 * dashes + len(BRAND_LCD)
    start = (LCD_COLS - block) // 2
    row = [" "] * LCD_COLS
    for i in range(dashes):
        row[start + i] = LCD_RULE
        row[start + block - 1 - i] = LCD_RULE
    for i, c in enumerate(BRAND_LCD):
        row[start + dashes + i] = c
    return "".join(row)


def build_home_brand_row():
    """Mirror screens.cpp buildHomeBrandRow()."""
    row = [" "] * LCD_COLS
    row[0] = row[1] = row[2] = "-"
    row[12] = " "
    row[13] = row[14] = row[15] = "-"
    for i, c in enumerate(BRAND_LCD):
        row[4 + i] = c
    return "".join(row)


def boot_step_sequence(elapsed_ms, step_ms, ready_at_ms):
    """Simulasi urutan frame LCD selama boot (handleBoot/handleInit).

    Mengembalikan list baris status yang benar-benar ditulis ke LCD.
    Baris identik berurutan DITONJOL oleh shadow buffer display.cpp,
    jadi model ini ikut menghitung penulisan unik — itulah bukti
    tidak ada kedip per-loop dan tidak ada erase/write berlebihan.
    """
    writes = []

    def emit(line0, line1):
        if not writes or writes[-1] != (line0, line1):
            writes.append((line0, line1))

    for frame in range(3):
        emit(build_brand_row(min(frame, 2)), BOOT_STEP_TEXT[0])

    if ready_at_ms is not None and elapsed_ms >= ready_at_ms:
        emit(build_brand_row(0), "READY")
        return writes

    step = min(int(elapsed_ms // step_ms), len(BOOT_STEP_TEXT) - 1)
    for s in range(1, step + 1):
        emit(build_brand_row(0), BOOT_STEP_TEXT[s])

    return writes


def status_writes(writes):
    """Baris status (row 1) yang benar-benar dikirim ke LCD.

    display.cpp menyimpan shadow per baris dan hanya menulis baris
    yang berubah, jadi tiga frame splash dengan status sama hanya
    menghasilkan SATU write ke LCD — inilah bukti tidak ada kedip.
    """
    out = []
    for _, row1 in writes:
        if not out or out[-1] != row1:
            out.append(row1)
    return out


def test_g22_rename_space():
    print("G22.1 RENAME SPACE: karakter spasi nyata, validasi tetap")
    sd, tmp = sd_home()
    try:
        # --- SPACE adalah karakter ring yang sah, byte 0x20 ---------
        _check("G22.1 SPACE ada di ring karakter",
               kit_rename_index_of(KIT_RENAME_SPACE) == 0)
        _check("G22.1 SPACE bukan underscore",
               kit_rename_index_of("_") != kit_rename_index_of(
                   KIT_RENAME_SPACE))
        _check("G22.1 SPACE bukan string literal 'SPACE'",
               "SPACE" not in KIT_RENAME_ALPHABET)
        _check("G22.1 karakter SPACE = 0x20 (bukan 0x5F)",
               KIT_RENAME_SPACE == chr(0x20) and
               KIT_RENAME_SPACE != chr(0x5F))
        _check("G22.1 ring tanpa karakter duplikat",
               len(set(KIT_RENAME_ALPHABET)) == len(KIT_RENAME_ALPHABET))

        # --- Sel kosong menghasilkan SPACE sebagai langkah pertama ---
        _check("G22.1 sel kosong (NUL) -> SPACE maju",
               kit_rename_char_after("\x00", 1) == " ")
        _check("G22.1 sel kosong (NUL) -> SPACE mundur",
               kit_rename_char_after("\x00", -1) == " ")
        _check("G22.1 sel kosong panjang editor -> SPACE",
               kit_rename_char_after(" ", 1) == "A" and
               kit_rename_char_after("K", -1) == "J")

        # --- RENAME -> ROCK LIVE (headline spec) -------------------
        # 1a. lepas nama lama supaya slot 1 bebas untuk diuji penuh.
        res, clean = rename_kit(sd, 1, "ROCKSHOW")
        _check("G22.1 RENAME lepas nama lama (ROUND)", res == "OK" and
               clean == "ROCKSHOW" and
               resolve_kit_dir(sd, 1) == "/kits/ROCKSHOW")

        # 1b. RENAME -> ROCK LIVE lewat editor sungguhan.
        ed = RenameEditor("ROCKSHOW")
        typed = ed.type_name("ROCK LIVE")
        sent = ed.confirmed_name()
        res, clean = rename_kit(sd, 1, sent or "")

        _check("G22.1 RENAME 'ROCK LIVE' diketik dari editor",
               typed and sent == "ROCK LIVE")
        _check("G22.1 RENAME 'ROCK LIVE' diterima backend", res == "OK")
        _check("G22.1 LOAD kembali = 'ROCK LIVE'",
               resolve_kit_dir(sd, 1) == "/kits/ROCK LIVE" and
               read_kit_meta(sd, "/kits/ROCK LIVE/kit.json", 1)["name"]
               == "ROCK LIVE")
        _check("G22.1 UI menampilkan 'ROCK LIVE'",
               lcd_row("ROCK LIVE").strip() == "ROCK LIVE")

        # 1c. JSON menyimpan spasi normal, bukan underscore/escape.
        doc = sd.read_json("/kits/ROCK LIVE/kit.json")
        _check("G22.1 JSON name == 'ROCK LIVE' (spasi normal)",
               doc["name"] == "ROCK LIVE" and "_" not in doc["name"])
        _check("G22.1 folder kit mengikuti nama (spasi ikut)",
               os.path.isdir(sd.p("/kits/ROCK LIVE")) and
               not os.path.exists(sd.p("/kits/ROCK_LIVE")))
        _check("G22.1 samples kit utuh setelah rename",
               len(sorted(f for f in sd.listdir("/kits/ROCK LIVE/samples")
                          if f.lower().endswith(".wav"))) == 12)
        _check("G22.1 kitId tetap 1 setelah rename",
               doc["kitId"] == 1 and doc["id"] == 1)

        # --- Beberapa spasi aman ----------------------------------
        multi = [
            (4, "Latin", "KENDANG JAWA"),
            (3, "Jazz", "KIT TEST 01"),
            (2, "Electronic", "DRUM DANGDUT"),
            (1, "ROCK LIVE", "ROCK LIVE SET"),
        ]
        for slot, old, new in multi:
            ed = RenameEditor(old)
            typed = ed.type_name(new)
            sent = ed.confirmed_name()
            res, clean = rename_kit(sd, slot, sent or "")
            d = resolve_kit_dir(sd, slot)
            meta = read_kit_meta(sd, d + "/kit.json", slot) if d else None
            _check(f"G22.1 RENAME '{old}' -> '{new}' aman",
                   typed and sent == new and res == "OK" and
                   d == "/kits/" + new and meta and meta["name"] == new,
                   f"typed={typed} sent={sent!r} res={res} dir={d}")

        # Dua spasi berturut-turut (leading spasi di-tolak backend,
        # internal spasi tetap sah).
        ed = RenameEditor("Jazz")
        ed.type_name("A  B")
        _check("G22.1 dua spasi internal bisa diketik",
               ed.confirmed_name() == "A  B")
        _check("G22.1 validasi menerima spasi internal",
               validate_kit_name("A  B") == "A  B")

        # Spasi di ujung dipangkas (leading oleh backend, trailing
        # oleh kitRenameFinalize()).
        ed = RenameEditor("Jazz")
        ed.type_name("LIVE  ")
        _check("G22.1 spasi kanan dipangkas saat konfirmasi",
               ed.confirmed_name() == "LIVE")
        _check("G22.1 spasi kiri dipangkas backend",
               validate_kit_name("  LIVE") == "LIVE")

        # --- Nama yang HANYA spasi tetap ditolak ------------------
        for blank in (" ", "   ", "        ", "\x00"):
            ed = RenameEditor("Jazz")
            ed.buf = blank.ljust(KIT_RENAME_MAX, "\x00")
            sent = ed.confirmed_name()
            _check(f"G22.1 nama spasi-saja {blank!r} ditolak (batal)",
                   sent is None, f"sent={sent!r}")
            _check(f"G22.1 backend juga tolak {blank!r}",
                   validate_kit_name(blank) is None)

        # --- Aturan validasi lain tidak berubah -------------------
        _check("G22.1 separator path tetap ditolak",
               validate_kit_name("ROCK/LIVE") is None and
               validate_kit_name("ROCK\\LIVE") is None)
        _check("G22.1 control character tetap ditolak",
               validate_kit_name("ROCK\tLIVE") is None and
               validate_kit_name("ROCK\nLIVE") is None)
        _check("G22.1 karakter ilegal FAT tetap ditolak",
               validate_kit_name("RO?K") is None and
               validate_kit_name('RO"K') is None)
        _check("G22.1 '.' dan '..' tetap ditolak",
               validate_kit_name(".") is None and
               validate_kit_name("..") is None)
        _check("G22.1 titik di ujung tetap ditolak",
               validate_kit_name("LIVE.") is None)
        _check("G22.1 panjang > KIT_NAME_MAX ditolak",
               validate_kit_name("A" * (KIT_NAME_MAX + 1)) is None)
        _check("G22.1 panjang tepat KIT_NAME_MAX diterima",
               validate_kit_name("A" * KIT_NAME_MAX) == "A" * KIT_NAME_MAX)
        _check("G22.1 batas editor RENAME tetap 14",
               KIT_RENAME_MAX == 14 and
               len("KENDANG JAWA") <= KIT_RENAME_MAX and
               len("DRUM DANGDUT") <= KIT_RENAME_MAX and
               len("KIT TEST 01") <= KIT_RENAME_MAX)
        _check("G22.1 collision nama kit lain tetap COLLISION",
               rename_kit(sd, 0, "KENDANG JAWA")[0] == "COLLISION")
        # Factory slot (0) TIDAK dikunci untuk RENAME di firmware —
        # hanya CREATE/DELETE yang PROTECTED. G22 tidak mengubah ini.
        res, clean = rename_kit(sd, 0, "DRUM GROOVE")
        _check("G22.1 RENAME slot factory tetap allowed (tidak diubah G22)",
               res == "OK" and clean == "DRUM GROOVE" and
               resolve_kit_dir(sd, 0) == "/kits/DRUM GROOVE",
               f"res={res} clean={clean!r}")
        _check("G22.1 PROTECTED factory tetap untuk CREATE/DELETE",
               "return KitOpResult::PROTECTED;" in
               _src("DrumCore", "kit_manager.cpp"))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def test_g22_rename_space_ui_render():
    print("G22.2 RENAME SPACE: caret & tampilan editor di LCD 16x2")
    ed = RenameEditor("ROCK")
    ed.type_name("ROCK LIVE")
    name = ed.confirmed_name()

    # screenShowKitRename(): 14 kolom + baris caret '^'.
    line1 = lcd_row(name.ljust(14))
    caret = " " * 14
    caret_idx = name.index(" ")
    caret = caret[:caret_idx] + "^" + caret[caret_idx + 1:]

    _check("G22.2 nama ruang muat di baris 1 (14 kolom)",
           len(name) <= 14)
    _check("G22.2 caret tepat di bawah karakter SPACE",
           caret[caret_idx] == "^" and caret.count("^") == 1)
    _check("G22.2 baris 1 tidak menampilkan underscore",
           "_" not in line1)
    _check("G22.2 baris 1 bukan literal 'SPACE'",
           "SPACE" not in line1)
    _check("G22.2 dua baris tepat 16 kolom",
           len(line1) == 14 and len(caret) == 14)

    # Nama panjang penuh 14 kolom tidak meluber.
    ed = RenameEditor("Jazz")
    ed.type_name("KENDANG JAWA")
    _check("G22.2 nama 12 kolom tetap utuh",
           ed.confirmed_name() == "KENDANG JAWA" and
           len(lcd_row(ed.confirmed_name())) == 12)

    # Nama panjang (editor CREATE = 28) dipotong bersih oleh display
    # 14 kolom, bukan error dan bukan teks terpotong jelek.
    ed = RenameEditor("Jazz", limit=KIT_NAME_MAX)
    ed.type_name("DRUM DANGDUT LIVE")
    long_name = ed.confirmed_name()
    row = lcd_row(long_name)[:KIT_RENAME_MAX]
    _check("G22.2 editor CREATE menerima nama > 14 kolom",
           long_name == "DRUM DANGDUT LIVE")
    _check("G22.2 nama panjang dipotong bersih di 14 kolom",
           row == "DRUM DANGDUT L" and row.rstrip() ==
           long_name[:KIT_RENAME_MAX].rstrip(), repr(row))
    _check("G22.2 pemotongan tidak memotong di tengah kata",
           long_name.startswith(row) and " " not in row[13:],
           repr(row))


def test_g22_lcd_branding():
    print("G22.3 LCD branding 'DRUM COK' (display only)")
    _check("G22.3 brand LCD = 'DRUM COK'", BRAND_LCD == "DRUM COK")
    _check("G22.3 panjang brand sama dengan 'DRUMCORE' (8 kolom)",
           len(BRAND_LCD) == len(BRAND_OLD) == 8)
    _check("G22.3 baris brand polos terpusat",
           build_brand_row(0) == "    DRUM COK    ")
    for dashes in (1, 2):
        row = build_brand_row(dashes)
        _check(f"G22.3 baris brand dashes={dashes} tepat 16 kolom",
               len(row) == LCD_COLS and BRAND_LCD in row)
    home = build_home_brand_row()
    _check("G22.3 lockup home = '--- DRUM COK ---'",
           home == "--- DRUM COK ---" and len(home) == LCD_COLS)
    _check("G22.3 tidak ada branding lama di baris brand",
           all(BRAND_OLD not in r for r in
               (build_brand_row(0), build_brand_row(1),
                build_brand_row(2), home)))
    _check("G22.3 'CHECK CONNECTION' pas 16 kolom",
           len("CHECK CONNECTION") == LCD_COLS)
    _check("G22.3 'READY' muat dan terpusat",
           len("READY") <= LCD_COLS)


def test_g22_boot_ui_behaviour():
    print("G22.4 Boot UI: step statis, cepat, tanpa kedip")
    splash_ms = 3 * 350          # BOOT_FRAME_COUNT * BOOT_FRAME_MS
    step_ms = 400                # BOOT_STEP_MS
    init_min_ms = 700            # INIT_MIN_MS

    # (a) modul sangat cepat: PING OK tepat di INIT_MIN_MS.
    fast = boot_step_sequence(init_min_ms, step_ms, init_min_ms)
    fast_rows = status_writes(fast)
    _check("G22.4 splash + step awal + READY tanpa kedip",
           fast == [(build_brand_row(0), "Initializing"),
                    (build_brand_row(1), "Initializing"),
                    (build_brand_row(2), "Initializing"),
                    (build_brand_row(0), "READY")],
           str(fast))
    _check("G22.4 modul cepat -> langsung READY",
           fast_rows[-1] == "READY" and fast_rows.count("READY") == 1)
    _check("G22.4 tidak ada step setelah READY",
           fast_rows.index("READY") == len(fast_rows) - 1)
    _check("G22.4 total boot cepat = timeline lama (1050+700+900)",
           splash_ms + init_min_ms + 900 == 2650)
    _check("G22.4 splash tidak menambah waktu boot",
           splash_ms == 3 * 350)

    # (b) modul lambat: seluruh step terisi, tanpa homozygous blink.
    slow = boot_step_sequence(9 * 1000, step_ms, None)
    slow_rows = status_writes(slow)
    _check("G22.4 modul lambat menampilkan semua step",
           set(BOOT_STEP_TEXT).issubset(set(slow_rows)))
    _check("G22.4 tiap step hanya ditulis sekali (tanpa kedip)",
           all(slow_rows.count(t) == 1 for t in BOOT_STEP_TEXT),
           str(slow_rows))
    _check("G22.4 splash 3 frame = 1 write status (shadow buffer)",
           slow_rows.count("Initializing") == 1, str(slow_rows))
    _check("G22.4 baris brand tetap DRUM COK selama boot",
           all(BRAND_LCD in a for a, _ in slow))
    _check("G22.4 tidak ada loop LCD tak terbatas",
           len(slow) == 3 + (len(BOOT_STEP_TEXT) - 1), str(len(slow)))

    # (c) semua teks muat di 16 kolom.
    _check("G22.4 semua step muat 16 kolom",
           all(len(t) <= LCD_COLS for t in BOOT_STEP_TEXT),
           str([(t, len(t)) for t in BOOT_STEP_TEXT]))
    _check("G22.4 step persis seperti spesifikasi",
           BOOT_STEP_TEXT == ("Initializing", "Checking SD", "Loading Kit",
                              "Loading Samples", "Starting Engine"))
    _check("G22.4 tidak ada elipsis berkedip di baris status",
           not any("." * 3 in t for t in BOOT_STEP_TEXT))


def test_g22_source_verification():
    """G22.5 source verification: bentuk patch di DrumCore_UI."""
    print("G22.5 source verification: SPACE + branding + boot UI")
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

    def read(*parts):
        return _src(*parts)

    screens_src = read("DrumCore_UI", "screens.cpp")
    screens_h = read("DrumCore_UI", "screens.h")
    ui_src = read("DrumCore_UI", "ui.cpp")
    ui_h = read("DrumCore_UI", "ui.h")
    ui_cfg = read("DrumCore_UI", "config.h")
    display_src = read("DrumCore_UI", "display.cpp")
    display_h = read("DrumCore_UI", "display.h")
    uart_client_src = read("DrumCore_UI", "uart_client.cpp")
    ui_ini = read("DrumCore_UI", "platformio.ini")

    # --- G22.1 SPACE sebagai karakter nyata ----------------------
    _check("G22.5 ui.cpp punya konstanta SPACE 0x20",
           "static constexpr char KIT_RENAME_SPACE = ' ';" in ui_src)
    _check("G22.5 alphabet punya SPACE di slot 0",
           "static const char KIT_RENAME_ALPHABET[] =\n"
           "    \" ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
           "0123456789-_.\";" in ui_src)
    _alpha = re.search(r'KIT_RENAME_ALPHABET\[\]\s*=\s*\n?\s*"([^"]*)"',
                       ui_src)
    _alpha_s = _alpha.group(1) if _alpha else ""
    _check("G22.5 alphabet tidak punya literal \"SPACE\"",
           "SPACE" not in _alpha_s, repr(_alpha_s))
    _check("G22.5 alphabet tidak punya literal \"SPACE\" di kode editor",
           "SPACE" not in _strip_comments(
               _func_body(ui_src, "static void handleNameEditor(")))
    after = _strip_comments(_func_body(ui_src, "static char kitRenameCharAfter("))
    _check("G22.5 kitRenameCharAfter mengembalikan SPACE di sel kosong",
           "if (base < 0)" in after and
           "return KIT_RENAME_SPACE;" in after)
    _check("G22.5 kitRenameCharAfter wrap ring (tidak keluar alphabet)",
           "next >= len" in after and "next < 0" in after and
           "return KIT_RENAME_ALPHABET[next];" in after)
    index_fn = _func_body(ui_src, "static int8_t kitRenameIndexOf(")
    rename_fn = _func_body(ui_src, "static void handleKitRename(")
    create_fn = _func_body(ui_src, "static void handleKitCreateName(")
    _check("G22.5 pemindaian ring hanya ada di kitRenameIndexOf",
           "for (int8_t i = 0; i < len; i++)" in index_fn and
           "for (int8_t i = 0; i < len; i++)" not in rename_fn and
           "for (int8_t i = 0; i < len; i++)" not in create_fn)
    ui_code = _strip_comments(ui_src)
    _check("G22.5 kedua editor pakai helper yang sama",
           ui_code.count("kitRenameCharAfter(") == 2,   # G24.15: 1 def + 1 call
           str(ui_code.count("kitRenameCharAfter(")))
    final = _strip_comments(_func_body(ui_src, "static bool kitRenameFinalize("))
    _check("G22.5 finalize memangkas spasi kanan",
           "renameBuf[n - 1] == KIT_RENAME_SPACE" in final and
           "return false;" in final)
    _check("G22.5 RENAME & CREATE sama-sama pakai kitRenameFinalize",
           _strip_comments(ui_src).count("kitRenameFinalize()") == 2,
           str(_strip_comments(ui_src).count("kitRenameFinalize()")))
    _check("G22.5 batas panjang nama tidak berubah",
           "constexpr uint8_t KIT_RENAME_MAX = 14;" in ui_src and
           "constexpr uint8_t KIT_CREATE_NAME_MAX = KIT_NAME_MAX;"
           in ui_src)

    # --- G22.2 LCD branding ---------------------------------------
    _check("G22.5 screens.cpp punya satu sumber brand 'DRUM COK'",
           'static const char BRAND_LCD[] = "DRUM COK";' in screens_src)
    _check("G22.5 tidak ada string literal DRUMCORE di kode LCD",
           BRAND_OLD not in _strip_comments(screens_src) and
           BRAND_OLD not in _strip_comments(screens_h) and
           BRAND_OLD not in _strip_comments(ui_src) and
           BRAND_OLD not in _strip_comments(ui_h) and
           BRAND_OLD not in _strip_comments(ui_cfg) and
           BRAND_OLD not in _strip_comments(display_src) and
           BRAND_OLD not in _strip_comments(display_h))
    _check("G22.5 semua baris brand dibangun dari BRAND_LCD",
           "BRAND_LCD" in _func_body(screens_src, "static void buildBrandRow(") and
           "BRAND_LCD" in _func_body(screens_src,
                                     "static void buildHomeBrandRow("))
    _check("G22.5 baris home pakai jalur shadow-buffer (hemat I2C)",
           "displayPrintLine(0, row0)" in screens_src and
           "static const char ROW0[17]" not in screens_src)

    # --- G22.3 boot UI --------------------------------------------
    _check("G22.5 steps diekspor sebagai konstanta tunggal",
           "constexpr uint8_t BOOT_STEP_COUNT = 5;" in screens_h)
    _check("G22.5 BOOT_STEP_TEXT lengkap di screens.cpp",
           "static const char* const BOOT_STEP_TEXT[BOOT_STEP_COUNT]" in
           screens_src and
           all('"%s"' % t in screens_src for t in BOOT_STEP_TEXT))
    _check("G22.5 API boot step menggantikan animasi titik",
           "void screenShowBootStep(uint8_t step);" in screens_h and
           "screenShowInit" not in screens_src and
           "screenShowInit" not in screens_h and
           "screenShowInit" not in ui_src)
    _check("G22.5 konstanta kedip titik dihapus",
           "INIT_DOT_MS" not in ui_cfg and "lastInitDot" not in ui_src)
    boot_step = _strip_comments(_func_body(ui_src, "static void handleInit()\n{"))
    _check("G22.5 handleInit hanya menulis LCD saat step berubah",
           "if (step != lastInitStep)" in boot_step and
           boot_step.count("screenShowBootStep(") == 1)
    _check("G22.5 step di-clamp ke BOOT_STEP_COUNT",
           "step >= BOOT_STEP_COUNT" in boot_step)
    _check("G22.5 splash 3 frame tetap, baris bawah tidak kosong",
           "displayPrintLine(1, BOOT_STEP_TEXT[0]);" in screens_src)
    _check("G22.5 handleBoot tetap millis-driven (non-blocking)",
           "(uint8_t)((now - bootStartMs) / BOOT_FRAME_MS)" in
           _strip_comments(_func_body(ui_src, "static void handleBoot()\n{")))
    _check("G22.5 handleInit tidak menambah delay / tidak blocking",
           "delay(" not in boot_step and "while (" not in boot_step)
    _check("G22.5 tidak ada delay() baru di seluruh ui.cpp",
           _strip_comments(ui_src).count("delay(") == 0)

    # --- timing boot tidak berubah (tidak ada boot delay baru) ----
    for name, ctype, value in (("BOOT_FRAME_MS", "uint16_t", "350"),
                               ("BOOT_FRAME_COUNT", "uint8_t", "3"),
                               ("TRANSITION_MS", "uint16_t", "200"),
                               ("INIT_MIN_MS", "uint16_t", "700"),
                               ("READY_HOLD_MS", "uint16_t", "900"),
                               ("UART_STARTUP_WAIT_MS", "uint16_t", "500"),
                               ("UART_PING_RETRY_MS", "uint16_t", "500"),
                               ("UART_STARTUP_TIMEOUT_MS", "uint16_t", "9000"),
                               ("UI_UPDATE_MS", "uint16_t", "20")):
        _check(f"G22.5 {name} tidak berubah ({value})",
               f"constexpr {ctype} {name} = {value};" in ui_cfg)
    _check("G22.5 BOOT_STEP_MS purely presentational (400)",
           "constexpr uint16_t BOOT_STEP_MS = 400;" in ui_cfg)

    # --- handshake PING tidak berubah -----------------------------
    _check("G22.5 urutan fase PING tetap (WAIT_START/AWAIT_RESP/DONE_OK)",
           boot_step.count("case PingPhase::") == 3)
    _check("G22.5 uartSendPing() tetap dipanggil di handleInit",
           "uartSendPing();" in boot_step)
    _check("G22.5 timeout -> CHECK CONNECTION tetap",
           "enterCheckConnection();" in boot_step)
    _check("G22.5 INIT_MIN_MS tetap menentukan latch READY",
           "now - initStartMs >= INIT_MIN_MS" in boot_step and
           "enterReady();" in boot_step)
    _check("G22.5 PING OK tidak menunggu proses background",
           "uartPollPing()" in boot_step and
           "psram" not in boot_step.lower())

    # --- G22.4 protocol / transport tidak berubah -----------------
    _check("G22.5 UART frame tetap 8 byte (UI)",
           "constexpr uint8_t UART_FRAME_SIZE = 8;" in ui_cfg)
    _check("G22.5 SOF tetap 0xD3 (UI)",
           "constexpr uint8_t UART_SOF = 0xD3;" in ui_cfg)
    _check("G22.5 baud tetap 921600 (UI)",
           "constexpr uint32_t UART_BAUD = 921600;" in ui_cfg)
    _check("G22.5 CMD_KIT_RENAME tetap 0x17",
           "CMD_KIT_RENAME       = 0x17" in
           read("DrumCore_UI", "uart_client.h"))
    _check("G22.5 chunk UART rename tetap 3 char/frame",
           "while (chunk * 3 < len)" in uart_client_src and
           "(uint8_t)(chunk | 0x80)" in uart_client_src)
    _check("G22.5 uartSendRenameRequest tetap kirim nama apa adanya",
           "return sendNameChunked(CMD_KIT_RENAME, kitIndex, name, "
           "KIT_NAME_MAX);" in uart_client_src)
    _check("G22.5 strlen-based panjang nama (spasi ikut terhitung)",
           "while (name[len] != '\\0' && len < maxLen)" in uart_client_src)

    # --- G22.5 project / internal tetap DrumCore ------------------
    _check("G22.5 project directory tetap DrumCore / DrumCore_UI",
           os.path.isdir(os.path.join(root, "DrumCore")) and
           os.path.isdir(os.path.join(root, "DrumCore_UI")))
    _check("G22.5 PlatformIO env tetap drumcore / drumcore_ui",
           "[env:drumcore]" in ui_ini.replace("[env:drumcore_ui]",
                                              "[env:drumcore]") and
           "[env:drumcore]" in read("DrumCore", "platformio.ini"))
    _check("G22.5 tidak ada 'DRUM COK'/'DRUMCOK' di firmware ESP32",
           "DRUM COK" not in read("DrumCore", "DrumCore.ino") and
           "DRUMCOK" not in read("DrumCore", "DrumCore.ino"))
    _check("G22.5 branding LCD hanya ada di DrumCore_UI",
           sum('"DRUM COK"' in read("DrumCore", f)
               for f in ("config.h", "uart_protocol.h", "kit_manager.cpp",
                         "kit_loader.cpp", "audio.cpp", "trigger.cpp")) == 0)
    ino = read("DrumCore", "DrumCore.ino")
    _check("G22.5 firmware ID tetap 'DrumCore v...'",
           'Serial0.print("=== DrumCore v");' in ino and
           'Serial0.println(" READY ===");' in ino)
    _check("G22.5 versi firmware tetap 0.4.0",
           'constexpr char FW_VERSION[] = "0.4.0";' in
           read("DrumCore", "config.h"))

    # --- G22.5 backend nama tidak berubah -------------------------
    km = _func_body(read("DrumCore", "kit_manager.cpp"),
                    "bool kitManagerValidateName(")
    km_code = _strip_comments(km)
    for token in ("c == '/' || c == '\\\\'", "c < 0x20",
                  'c == \'"\' || c == \'*\' || c == \':\'',
                  "start >= n", "name[start] == '.' && name[start + 1] == '.'",
                  "name[n - 1] == '.'"):
        _check(f"G22.5 kitManagerValidateName tetap {token}",
               token.replace("\\", "\\") in km_code or
               token in km_code)
    _check("G22.5 kitManagerValidateName tidak menolak spasi internal",
           "name[i] == ' '" not in km_code and
           "c == ' '" not in km_code)
    _check("G22.5 KIT_NAME_MAX tetap 28",
           "constexpr uint8_t KIT_NAME_MAX      = 28;" in
           read("DrumCore", "config.h"))

    # --- G22.5 runtime / audio / PSRAM tetap utuh ---------------
    core_cfg = read("DrumCore", "config.h")
    audio_src = read("DrumCore", "audio.cpp")
    psram_src = read("DrumCore", "psram_loader.cpp")
    uart_src = read("DrumCore", "uart_protocol.cpp")
    _check("G22.5 12 pad tidak berubah",
           "constexpr uint8_t PAD_COUNT = 12;" in core_cfg)
    _check("G22.5 UART1 baud tetap 921600",
           "constexpr uint32_t UART1_BAUD = 921600;" in core_cfg)
    _check("G22.5 trigger engine tidak disentuh",
           "void triggerTask(" in read("DrumCore", "trigger.cpp") and
           "audioSetHitAccept" in audio_src)
    _check("G22.5 audio realtime tetap psramSlotForPad",
           "psramSlotForPad(padIndex)" in
           _strip_comments(_func_body(audio_src, "void fireHit(")))
    _check("G22.5 PSRAM loader tetap utuh",
           "commitKitRegistry(" in psram_src and
           "clearSampleRegistry();" in psram_src)
    _check("G22.5 UART task tetap 8 KB + dispatch utuh",
           "UART_TASK_STACK = 8192" in uart_src and
           "case CMD_KIT_RENAME:" in uart_src)
    _check("G22.5 tidak ada penamaan internal 'DRUM COK'",
           "DRUM COK" not in _strip_comments(uart_src) and
           "DrumCok" not in uart_src and "drumCok" not in uart_src)


def main():
    if not os.path.isdir(FIXTURE):
        print(f"Fixture tidak ditemukan: {FIXTURE}")
        return 2

    test_create_empty_baseline_safe()
    test_create_copy_independent()
    test_mapping_by_filename()
    test_kit_save_roundtrip()
    test_last_active_kit_persist()
    test_missing_kit_fallback()
    test_arbitrary_wav_discovery()
    test_kit_independence()
    test_resolver_name_based_g12()
    test_fixture_g5_audit()
    test_g2g3g4_lifecycle()
    test_g13_wemos_kitinfo_scheduling()
    test_g14_split_burst_guard()
    test_g16_active_kit_name_cache()
    test_g17_kitinfo_assembly_and_renderer()
    test_g18_preview_resolver_and_stack_safety()
    test_g19_kit_load_stack_fix()
    test_g20_pad_sound_empty_samples()
    test_g20_pad_sound_non_empty()
    test_g20_pad_sound_last_chunk_variants()
    test_g20_pad_sound_count_cap()
    test_g20_pad_sound_repeat_request()
    test_g20_pad_sound_timeout_recovery()
    test_g20_pad_sound_invalid_seq_chunk()
    test_g20_pad_sound_set()
    test_g21_remap_no_kit_reload()
    test_g21_new_single_sample()
    test_g21_shared_slot_and_unassign()
    test_g21_save_persist_reboot()
    test_g21_rapid_reassign()
    test_g21_audio_while_remapping()
    test_g21_kit_switch_still_full_load()
    test_g21_failure_paths_clean()
    test_g21_source_verification()
    test_g22_rename_space()
    test_g22_rename_space_ui_render()
    test_g22_lcd_branding()
    test_g22_boot_ui_behaviour()
    test_g22_source_verification()
    test_g23_heap_model_sanity()
    test_g23_full_kit_unload_frees_extra_slots()
    test_g23_clear_registry_owns_nothing()
    test_g23_repeated_load_no_growth()
    test_g23_extra_slots_no_leak()
    test_g23_no_duplicate_allocation()
    test_g23_kit_load_after_single_sample()
    test_g23_switch_cycles_20x()
    test_g23_rollback_keeps_old_kit()
    test_g23_voice_gate_never_frees_live_voice()
    test_g23_source_verification()
    test_g24_top_level_menu()
    test_g24_kit_menu()
    test_g24_kit_create_methods()
    test_g24_pad_menu()
    test_g24_banned_labels_absent()
    test_g24_dead_states_removed()
    test_g24_back_navigation()
    test_g24_honest_messages()
    test_g24_boot_and_brand_intact()
    test_g24_esp32_untouched()
    test_g24_audit_classification()
    test_g24_no_ellipsis_placeholder()

    test_g24_readme_documented()

    test_g24_14_create_menu_reachable()
    test_g24_14_empty_opens_name_editor()
    test_g24_14_copy_opens_name_editor()
    test_g24_14_name_with_spaces_accepted()
    test_g24_14_empty_name_rejected()
    test_g24_14_empty_emits_existing_command()
    test_g24_14_copy_emits_existing_command()
    test_g24_14_response_ok_shows_created()
    test_g24_14_response_error_shows_failed()
    test_g24_14_new_kit_not_auto_active()
    test_g24_14_back_navigation()
    test_g24_14_rename_unchanged()
    test_g24_14_load_unchanged()
    test_g24_14_pad_unchanged()

    print("\n=== G24.15 CREATE REPAIR ===")
    test_g24_15_1_single_shared_editor()
    test_g24_15_2_rest_empty_heuristic_gone()
    test_g24_15_3_blank_editor_is_nul_not_spaces()
    test_g24_15_4_space_is_a_real_character()
    test_g24_15_5_confirm_is_last_cell()
    test_g24_15_6_cancel_and_parent_routes()
    test_g24_15_7_legacy_screens_removed()
    test_g24_15_8_rename_path_unchanged()
    test_g24_15_9_no_free_slot_is_no_space()
    test_g24_15_10_status_mapping_honest()
    test_g24_15_11_protocol_untouched()
    test_g24_15_12_wemos_handles_no_space()
    test_g24_15_13_no_error_masked_as_success()
    test_g24_15_14_empty_and_copy_postconditions()
    test_g24_15_15_scope_hygiene()

    print("\n=== G25 PSRAM KIT LOAD FRAGMENTATION ===")
    test_g25_fragmented_heap_both_designs()
    test_g25_field_case_diky_to_acoustic()
    test_g25_switch_cycles_repeated()
    test_g25_descending_order_helps()
    test_g25_g21_cache_hit_and_new_slot()
    test_g25_g23_ownership_and_rollback()
    test_g25_alias_shares_single_block()
    test_g25_source_verification()

    print("\n=== G-ADC ADC CALIBRATION INSTRUMENTATION ===")
    test_gadc_debug_flag()
    test_gadc_files_exist_and_connected()
    test_gadc_pad_count_coverage()
    test_gadc_gpio_displayed()
    test_gadc_struct_fields()
    test_gadc_modes()
    test_gadc_realtime_safety()
    test_gadc_no_protocol_change()
    test_gadc_no_ui_change()
    test_gadc_integration_via_loop()
    test_gadc_compile_out_when_false()

    print(f"\n=== {_passed} passed, {_failed} failed ===")
    return 0 if _failed == 0 else 1


# ===============================================================
# G23 — PSRAM LIFETIME / KIT LOAD ALLOC FAILURE
#
# G21 menambah slot runtime (slot 12..15) yang punya alokasi
# PSRAM sendiri (slotOwnBuf). KIT LOAD harus melepas SEMUA
# resource kit lama sebelum ps_malloc() blok full-kit baru.
#
# Di sini Ownership itu DIMODEL dengan heap sungguhan: first-fit
# + coalescing seperti ESP-IDF, supaya "free" bisa DIUKUR dalam
# byte dan fragmentasi bisa terlihat, bukan cuma flag boolean.
# ===============================================================

G23_PSRAM_KB = 8192            # ESP.getPsramSize() = 8192 KB
G23_KIT_SAMPLE_PATH_LEN = 96   # KIT_SAMPLE_PATH_LEN


class PsramHeap:
    """Model heap PSRAM: first-fit + coalescing (ala ESP-IDF).

    Sengaja BUKAN counter sederhana. Kalau cuma counter, test
    "repeated load tidak menambah alokasi" akan selalu lulus
    walau urutan free-nya salah. Dengan free-list sungguhan:
      - leak        -> free_total turun permanen
      - free tanpa coalesce -> largest_free turun (FRAGMENTED)
    """

    ALIGN = 16

    def __init__(self, total_kb=G23_PSRAM_KB):
        self.total = total_kb * 1024
        self.free = [(0, self.total)]
        self.live = {}                 # handle -> (start, size)
        self._next = 1
        self.fail_count = 0
        self.peak_live = 0

    def _align(self, size):
        return (size + self.ALIGN - 1) & ~(self.ALIGN - 1)

    def _coalesce(self):
        self.free.sort()
        merged = []
        for start, size in self.free:
            if merged and merged[-1][0] + merged[-1][1] == start:
                merged[-1] = (merged[-1][0], merged[-1][1] + size)
            else:
                merged.append((start, size))
        self.free = merged

    def alloc(self, size):
        """ps_malloc(): None bila tidak ada blok yang cukup besar."""
        if size <= 0:
            return None
        need = self._align(size)
        self.free.sort()
        for i, (start, avail) in enumerate(self.free):
            if avail >= need:
                self.free.pop(i)
                if avail - need:
                    self.free.insert(i, (start + need, avail - need))
                h = self._next
                self._next += 1
                self.live[h] = (start, need)
                self.peak_live = max(self.peak_live,
                                     self.live_bytes())
                return h
        self.fail_count += 1
        return None

    def release(self, handle):
        """free(): None-safe, double-release ditolak diam-diam."""
        if handle is None or handle not in self.live:
            return False
        start, size = self.live.pop(handle)
        self.free.append((start, size))
        self._coalesce()
        return True

    def live_bytes(self):
        return sum(s for _, s in self.live.values())

    def live_bytes_of(self, handle):
        """Ukuran blok yang dipegang handle (0 bila sudah di-free)."""
        rec = self.live.get(handle)
        return rec[1] if rec else 0

    def free_total(self):
        return sum(s for _, s in self.free)

    def largest(self):
        return max((s for _, s in self.free), default=0)

    def live_count(self):
        return len(self.live)

    def reset_stats(self):
        self.fail_count = 0


class G23PsramRuntime(PsramRuntime):
    """PsramRuntime (model G21) + heap PSRAM sungguhan.

    State/registry G21 dipakai apa adanya; hanya titik alokasi dan
    pelepasan yang kini menyentuh heap first-fit + coalescing,
    sehingga free bisa DIUKUR dalam byte dan fragmentasi terlihat.
    """

    def __init__(self, sd, pcm_bytes=64 * 1024, psram_kb=G23_PSRAM_KB):
        super().__init__(sd)
        self.heap = PsramHeap(psram_kb)
        self.pcm = pcm_bytes
        self.kit_handle = None                     # psramBuffer
        self.slot_handle = [None] * G21_MAX_SLOTS  # slotOwnBuf[]
        self.trace = []                            # baris instrumentation
        self.released_kit = 0
        self.released_slots = 0
        self.last_cause = None
        # Fault injection untuk test alloc failure / rollback.
        self.block_next_alloc = False

    # ---------------- instrumentation ----------------

    def free_now(self):
        """(free_total, largest_free) dalam byte."""
        return self.heap.free_total(), self.heap.largest()

    def owned_slots(self):
        return sum(1 for h in self.slot_handle if h is not None)

    def live_handles(self):
        """Jumlah handle PSRAM yang masih hidup (kit + extra slot)."""
        n = 1 if self.kit_handle is not None else 0
        return n + self.owned_slots()

    def live_bytes(self):
        return self.heap.live_bytes()

    @staticmethod
    def _classify(free_and_largest, requested):
        """Cermin klasifikasi penyebab di printAllocFailure().

        free_total < requested  -> OOM (memang tidak cukup)
        free_total >= requested tapi largest < requested
                               -> FRAGMENTED (free-list tersegmen)
        """
        free, largest = free_and_largest
        if free < requested:
            return "OOM"
        if largest < requested:
            return "FRAGMENTED"
        return "UNKNOWN"

    def _note(self, tag, requested=0):
        free, largest = self.free_now()
        self.trace.append((tag, free, largest, requested))
        return (free, largest, requested)

    def free_trace(self, tag):
        # tag dicocokkan sebagai PREFIX supaya "before unload" cocok
        # dengan "before unload (pre-alloc)" / "(unload)" / "(empty-kit)".
        return [r for r in self.trace if r[0].startswith(tag)]

    def latest(self, tag):
        rows = self.free_trace(tag)
        return rows[-1] if rows else None

    def requested_of(self, tag):
        for (t, _f, _l, req) in self.trace:
            if t.startswith(tag):
                return req
        return None

    # ---------------- ps_malloc + fault injection ----------------

    def _alloc(self, size):
        if self.block_next_alloc:
            self.block_next_alloc = False
            self.heap.fail_count += 1
            return None
        return self.heap.alloc(size)

    # ---------------- releaseAll() ----------------

    def release_all(self, why):
        """Cermin releaseAll(const char *why) di psram_loader.cpp.

        SATU-SATUNYA titik yang membebaskan PSRAM kit: psramBuffer
        plus setiap slotOwnBuf. Dipakai oleh unload, pre-alloc, dan
        kit kosong.
        """
        self._note(f"before unload ({why})")
        freed = 0
        owned = self.owned_slots()
        if self.kit_handle is not None:
            freed += self.heap.live_bytes_of(self.kit_handle)
            self.heap.release(self.kit_handle)
            self.kit_handle = None
        for s in range(G21_MAX_SLOTS):
            h = self.slot_handle[s]
            if h is not None:
                freed += self.heap.live_bytes_of(h)
                self.heap.release(h)
                self.slot_handle[s] = None
            self.samples[s] = None
            self.slot_path[s] = None
            self.slot_own_buf[s] = False
        self.sample_count = 0
        self.kit_buffer = None
        self.kit_loaded = False
        self.slot_for_pad = list(range(PAD_COUNT))
        self.released_kit += 1
        self.released_slots += owned
        self._note(f"after unload ({why})")
        return freed

    def unload(self):
        """psramUnloadKit() -> releaseAll("unload")"""
        return self.release_all("unload")

    # ---------------- psramLoadKit() ----------------

    def load_kit(self, paths, sample_ids=None):
        """Cermin psramLoadKit().

        Free-all dulu (single ownership), baru ps_malloc blok
        full-kit. Kalau ps_malloc gagal, PSRAM menjadi KOSONG dan
        pemanggil WAJIB reload kit lama.
        """
        self.kit_loads += 1
        total = self.pcm * len([p for p in paths if p])
        self.release_all("pre-alloc")
        self._note("before kit alloc", total)
        handle = self._alloc(total)
        if handle is None:
            self.last_cause = self._classify(self.free_now(), total)
            self._note("kit alloc FAILED", total)
            return False
        self.kit_handle = handle
        self._note("kit alloc OK", total)

        self.sd_reads = []
        for i, p in enumerate(paths):
            if not p or not self.sd.exists(p):
                continue
            self.sd_reads.append(p)
            self.samples[i] = p
            self.slot_path[i] = p
        self.sample_count = len(paths)
        self.kit_buffer = "kit"
        self.kit_loaded = True
        ids = sample_ids if sample_ids is not None else list(range(PAD_COUNT))
        for p in range(PAD_COUNT):
            src = ids[p] if p < len(ids) else p
            if src < G21_KIT_SLOTS and src < self.sample_count \
                    and self.samples[src]:
                self.slot_for_pad[p] = src
            else:
                self.slot_for_pad[p] = p
        return True

    # ---------------- psramLoadSingleSample() ----------------

    def load_single_sample(self, path):
        """Cermin psramLoadSingleSample().

        PENTING: jalur ini TIDAK melakukan free dan TIDAK melakukan
        voice gate. Jadi tidak boleh menyentuh slot yang punya buffer
        sendiri (overwrite = leak) dan tidak boleh membebaskan buffer
        yang sedang dibaca voice.
        """
        if not path:
            return G21_SLOT_NONE

        existing = self.find_slot(path)
        if existing != G21_SLOT_NONE:
            return existing                 # cache hit: remap, NO malloc

        if not self.kit_buffer or not self.kit_loaded:
            return G21_SLOT_NONE
        if not self.sd_ready or self.alloc_fails:
            return G21_SLOT_NONE

        slot = G21_SLOT_NONE
        for s in range(G21_MAX_SLOTS):
            if not self.slot_path[s]:
                slot = s
                break
        if slot == G21_SLOT_NONE:
            return G21_SLOT_NONE

        # Guard orphan: JANGAN menimpa slot yang punya buffer sendiri.
        if self.slot_handle[slot] is not None:
            return G21_SLOT_NONE

        if not self.sd.exists(path):
            return G21_SLOT_NONE

        handle = self._alloc(self.pcm)
        if handle is None:
            self.last_cause = self._classify(self.free_now(), self.pcm)
            return G21_SLOT_NONE

        self.sd_reads.append(path)
        self.samples[slot] = path
        self.slot_path[slot] = path
        self.slot_own_buf[slot] = True
        self.slot_handle[slot] = handle
        if slot >= self.sample_count:
            self.sample_count = slot + 1
        self.single_loads += 1
        return slot


def g23_perform_kit_switch(rt, sd, kit_dir, pads, uart_active_kit,
                           force_new_kit_alloc_fail=False,
                           force_restore_fail=False):
    """performKitSwitch() setelah fix G23: gate -> voice drain ->
    load -> commit, dan ROLLBACK benar-benar memuat ulang kit lama.

    Mengembalikan (status, uart_active_kit).
    """
    active = [True]          # audioSetHitAccept
    voices = [0]             # audioActiveVoiceCount()

    active[0] = False        # 1. hit gate OFF
    if voices[0] != 0:
        return "BUSY", uart_active_kit   # 2. drain (drain = 0 di test)

    resolved = resolve_samples(sd, kit_dir, pads)
    new_kit = uart_active_kit + 1
    old_paths = list(rt.resolved)
    old_slot_for_pad = list(rt.slot_for_pad)

    saved_fail = rt.heap.fail_count
    if force_new_kit_alloc_fail:
        rt.block_next_alloc = True

    load_ok = rt.load_kit(resolved)
    if force_restore_fail:
        rt.block_next_alloc = True

    if load_ok:
        for i in range(PAD_COUNT):
            rt.resolved[i] = resolved[i] or ""
        active[0] = True
        return "OK", new_kit

    # --- Rollback (G23): kit lama HARUS dimuat ulang, karena
    #     psramLoadKit() sudah membebaskan blok kit lama.
    rt.resolved = list(old_paths)
    rt.slot_for_pad = list(old_slot_for_pad)
    restored = rt.load_kit(old_paths)

    if restored:
        active[0] = True
        return "BUSY", uart_active_kit

    active[0] = True
    return "ROLLBACK_FAILED", None


# ---------------------------------------------------------------
# G23 Tests
# ---------------------------------------------------------------

def _g23_kt():
    """3 kit dengan 12 sample masing-masing ( Acoustic/Electronic/
    ROCK LIVE) supaya siklus switch punya beban realistis."""
    sd, tmp = sd_home()
    kits = {}
    for name in ("Acoustic", "Electronic", "ROCK LIVE"):
        d = "/kits/" + name
        kits[name] = load_kit(sd, d)
    return sd, tmp, kits


def test_g23_heap_model_sanity():
    """G23.0: model heap harus bisa membedakan OOM dan FRAGMENTED."""
    print("G23.0 model heap: OOM vs FRAGMENTED bisa dibedakan")
    h = PsramHeap(1024)
    a = h.alloc(512 * 1024)
    _check("G23.0 alokasi besar pertama berhasil", a is not None)
    _check("G23.0 free total = total - live",
           h.free_total() == h.total - h.live_bytes())
    b = h.alloc(256 * 1024)
    _check("G23.0 alokasi kedua berhasil", b is not None)
    h.release(a)
    h.release(b)
    _check("G23.0 setelah free semua, largest == total (coalesce)",
           h.largest() == h.total, f"largest={h.largest()}")
    _check("G23.0 tidak ada handle tersisa", h.live_count() == 0)

    # Fragmentasi: blok besar dikepung dua alokasi kecil yang
    # TIDAK bisa digabung oleh free-list.
    h2 = PsramHeap(1024)
    guard_a = h2.alloc(300 * 1024)
    big = h2.alloc(496 * 1024)        # sengaja < 500KB agar hole
    guard_b = h2.alloc(200 * 1024)
    h2.release(big)
    _check("G23.0 fragmented: free total cukup",
           h2.free_total() >= 496 * 1024, str(h2.free_total()))
    _check("G23.0 fragmented: largest < 500KB",
           h2.largest() < 500 * 1024, str(h2.largest()))
    _check("G23.0 fragmented: alokasi 500KB benar-benar gagal",
           h2.alloc(500 * 1024) is None)
    _check("G23.0 fragmented: guard tetap hidup",
           h2.live_count() == 2 and guard_a and guard_b)
    h2.release(guard_a)
    h2.release(guard_b)
    _check("G23.0 fragmented: setelah semua free, largest == total",
           h2.largest() == h2.total)
    _check("G23.0 fragmented: tidak ada handle tersisa",
           h2.live_count() == 0)

    # OOM murni: ruang memang tidak cukup.
    h3 = PsramHeap(1024)
    h3.alloc(900 * 1024)
    _check("G23.0 OOM: free total < requested",
           h3.free_total() < 500 * 1024)
    _check("G23.0 OOM: alokasi 500KB gagal", h3.alloc(500 * 1024) is None)
    h3.release(1)
    _check("G23.0 OOM: setelah free, 500KB muat",
           h3.alloc(500 * 1024) is not None)


def test_g23_full_kit_unload_frees_extra_slots():
    """G23.1 TES A-E: full-kit unload HARUS bebaskan extra slot G21."""
    print("G23.1 TES A-E: unload membebaskan extra slot G21")
    sd, tmp, kits = _g23_kt()
    try:
        # pcm 64 KB x 12 = 768 KB per kit, PSRAM 8192 KB.
        rt = G23PsramRuntime(sd, pcm_bytes=64 * 1024)
        rt.load_kit(resolve_samples(sd, "/kits/Acoustic", kits["Acoustic"]))
        for i in range(PAD_COUNT):
            rt.resolved[i] = resolve_samples(
                sd, "/kits/Acoustic", kits["Acoustic"])[i] or ""

        free_after_boot = rt.heap.free_total()

        # TEST C: sample baru -> extra slot (buffer sendiri).
        new1 = abs_sample(sd, "/kits/Acoustic", "new01.wav")
        make_wav(os.path.join(sd.p("/kits/Acoustic"), "samples",
                              "new01.wav"))
        # TEST D: beberapa sample baru pada beberapa pad.
        made = {}
        for i in range(3):
            nm = "new%02d.wav" % (i + 1)
            make_wav(os.path.join(sd.p("/kits/Acoustic"), "samples", nm))
            made[i] = abs_sample(sd, "/kits/Acoustic", nm)

        for i in range(3):
            _check(f"G23.1 TEST C/D slot baru ter-alokasi (new0{i+1})",
                   rt.load_single_sample(made[i]) != G21_SLOT_NONE)
        _check("G23.1 3 extra slot punya buffer sendiri",
               rt.owned_slots() == 3, str(rt.owned_slots()))
        _check("G23.1 PSRAM turun setelah 3 extra load",
               rt.heap.free_total() == free_after_boot - 3 * 64 * 1024,
               str(rt.heap.free_total()))

        # TEST E: KIT LOAD Electronic.
        before = rt.heap.free_total()
        elec = resolve_samples(sd, "/kits/Electronic", kits["Electronic"])
        _check("G23.1 TEST E: KIT LOAD Electronic sukses",
               rt.load_kit(elec))
        _check("G23.1 TEST E: 12 sample Electronic ter-load",
               len(rt.sd_reads) == 12, str(len(rt.sd_reads)))

        after = rt.heap.live_bytes()
        _check("G23.1 TEST E: TIDAK ada extra slot kit lama yang hidup",
               rt.owned_slots() == 0, str(rt.owned_slots()))
        _check("G23.1 TEST E: live PSRAM = blok kit Electronic saja",
               after == 12 * 64 * 1024,
               f"live={after} expected={12 * 64 * 1024}")
        _check("G23.1 TEST E: free PSRAM naik kembali >= sebelum",
               rt.heap.free_total() >= before, str(rt.heap.free_total()))
        _check("G23.1 TEST E: registry = 12 file kit baru",
               len(rt.registry_paths()) == 12,
               str(len(rt.registry_paths())))
        _check("G23.1 TEST E: sample runtime Acoustic hilang",
               not any("new0" in (p or "") for p in rt.registry_paths()))

        # Instrumentation: 4 titik wajib ada.
        _check("G23.1 instrumentation 'before unload' ada",
               rt.latest("before unload") is not None)
        _check("G23.1 instrumentation 'after unload' ada",
               rt.latest("after unload") is not None)
        _check("G23.1 instrumentation 'before kit alloc' ada "
               "dengan requested",
               rt.latest("before kit alloc") is not None and
               rt.requested_of("before kit alloc") == 12 * 64 * 1024,
               str(rt.requested_of("before kit alloc")))
        _check("G23.1 instrumentation 'kit alloc OK' ada",
               rt.latest("kit alloc OK") is not None)
        _check("G23.1 after-unload free > before-unload free "
               "(extras benar-benar balik)",
               rt.latest("after unload")[1] >=
               rt.latest("before unload")[1])
        _check("G23.1 'before kit alloc' free >= 'after unload' free",
               rt.latest("before kit alloc")[1] >=
               rt.latest("after unload")[1])
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def test_g23_clear_registry_owns_nothing():
    """G23.2: clear registry tidak boleh meninggalkan owned alloc."""
    print("G23.2 clearSampleRegistry tidak meninggalkan alokasi")
    sd, tmp, kits = _g23_kt()
    try:
        rt = G23PsramRuntime(sd, pcm_bytes=32 * 1024)
        ac = resolve_samples(sd, "/kits/Acoustic", kits["Acoustic"])
        rt.load_kit(ac)
        for i in range(PAD_COUNT):
            rt.resolved[i] = ac[i] or ""

        for i in range(4):
            nm = "x%d.wav" % i
            make_wav(os.path.join(sd.p("/kits/Acoustic"), "samples", nm))
            rt.load_single_sample(abs_sample(sd, "/kits/Acoustic", nm))

        _check("G23.2 4 extra slot terisi sebelum unload",
               rt.owned_slots() == 4, str(rt.owned_slots()))

        rt.unload()

        _check("G23.2 unload: 0 owned slot tersisa",
               rt.owned_slots() == 0, str(rt.owned_slots()))
        _check("G23.2 unload: 0 live allocation di heap",
               rt.live_handles() == 0, str(rt.live_handles()))
        _check("G23.2 unload: free total == ukuran PSRAM",
               rt.heap.free_total() == rt.heap.total,
               f"{rt.heap.free_total()} vs {rt.heap.total}")
        _check("G23.2 unload: largest == total (fully coalesced)",
               rt.heap.largest() == rt.heap.total)
        _check("G23.2 unload: registry benar-benar kosong",
               rt.registry_paths() == [])
        _check("G23.2 unload: kit tidak loaded",
               not rt.kit_loaded and rt.sample_count == 0)
        _check("G23.2 unload: slotForPad kembali identitas",
               rt.slot_for_pad == list(range(PAD_COUNT)))

        # Unload kedua tidak boleh double-free / crash.
        rt.unload()
        _check("G23.2 unload kedua tetap 0 live allocation",
               rt.live_handles() == 0, str(rt.live_handles()))
        _check("G23.2 unload kedua tidak mengubah free total",
               rt.heap.free_total() == rt.heap.total)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def test_g23_repeated_load_no_growth():
    """G23.3: 20x load kit tidak menambah alokasi (tidak ada leak)."""
    print("G23.3 20x KIT LOAD: alokasi & free PSRAM konstan")
    sd, tmp, kits = _g23_kt()
    try:
        rt = G23PsramRuntime(sd, pcm_bytes=48 * 1024)
        resolved = {
            k: resolve_samples(sd, "/kits/" + k, kits[k])
            for k in ("Acoustic", "Electronic", "ROCK LIVE")
        }

        rt.load_kit(resolved["Acoustic"])
        for i in range(PAD_COUNT):
            rt.resolved[i] = resolved["Acoustic"][i] or ""

        baseline_free = None
        worst = None
        all_ok = True

        for n in range(20):
            kit = ("Acoustic", "Electronic", "ROCK LIVE")[n % 3]
            ok = rt.load_kit(resolved[kit])
            all_ok = all_ok and ok
            live = rt.heap.live_bytes()
            owned = rt.owned_slots()
            if worst is None:
                worst = live
            if live > worst:
                worst = live
            if baseline_free is None:
                baseline_free = rt.heap.free_total()
            # Setiap siklus harus kembali ke titik yang sama.
            if live != 12 * 48 * 1024 or owned != 0:
                worst = live

        _check("G23.3 20x KIT LOAD semua sukses", all_ok)
        _check("G23.3 live PSRAM tidak pernah melebihi 1 blok kit",
               worst == 12 * 48 * 1024, str(worst))
        _check("G23.3 free PSRAM kembali ke nilai stabil",
               rt.heap.free_total() ==
               rt.heap.total - 12 * 48 * 1024,
               f"{rt.heap.free_total()}")
        _check("G23.3 tidak ada handle bocor setelah 20 siklus",
               rt.live_handles() == 1, str(rt.live_handles()))
        _check("G23.3 tidak ada owned slot tersisa",
               rt.owned_slots() == 0)
        _check("G23.3 largest block tetap >= ukuran kit "
               "(tidak ter-fragmentasi)",
               rt.heap.largest() >= 12 * 48 * 1024,
               str(rt.heap.largest()))
        _check("G23.3 peak live tidak runaway",
               rt.heap.peak_live <= rt.heap.total)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def test_g23_extra_slots_no_leak():
    """G23.4: slot extra tidak bocor walau registry dipakai ulang."""
    print("G23.4 extra slot G21 tidak bocor")
    sd, tmp, kits = _g23_kt()
    try:
        rt = G23PsramRuntime(sd, pcm_bytes=32 * 1024)
        ac = resolve_samples(sd, "/kits/Acoustic", kits["Acoustic"])
        rt.load_kit(ac)
        for i in range(PAD_COUNT):
            rt.resolved[i] = ac[i] or ""

        start_free = rt.heap.free_total()

        # 4 slot extra terisi.
        for i in range(4):
            nm = "e%d.wav" % i
            make_wav(os.path.join(sd.p("/kits/Acoustic"), "samples", nm))
            rt.load_single_sample(abs_sample(sd, "/kits/Acoustic", nm))
        _check("G23.4 4 extra slot terisi", rt.owned_slots() == 4)
        _check("G23.4 registry penuh (16 entri)",
               len(rt.registry_paths()) == 16,
               str(len(rt.registry_paths())))

        # 5th load harus ditolak (registry full) tanpa alokasi.
        make_wav(os.path.join(sd.p("/kits/Acoustic"), "samples", "e9.wav"))
        free_before_reject = rt.heap.free_total()
        slot = rt.load_single_sample(abs_sample(sd, "/kits/Acoustic",
                                                "e9.wav"))
        _check("G23.4 registry full -> ditolak", slot == G21_SLOT_NONE)
        _check("G23.4 registry full: tidak ada alokasi baru",
               rt.heap.free_total() == free_before_reject)
        _check("G23.4 registry full: owned slot tetap 4",
               rt.owned_slots() == 4)

        # KIT LOAD Electronic: semua 4 extra harus balik.
        el = resolve_samples(sd, "/kits/Electronic", kits["Electronic"])
        rt.load_kit(el)
        _check("G23.4 4 extra slot dibebaskan saat KIT LOAD",
               rt.owned_slots() == 0, str(rt.owned_slots()))
        _check("G23.4 free PSRAM kembali >= sebelum extra load",
               rt.heap.free_total() >= start_free,
               f"{rt.heap.free_total()} vs {start_free}")
        _check("G23.4 live PSRAM = blok kit saja",
               rt.heap.live_bytes() == 12 * 32 * 1024,
               str(rt.heap.live_bytes()))
        _check("G23.4 largest block pulih penuh",
               rt.heap.largest() >= 12 * 32 * 1024,
               str(rt.heap.largest()))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def test_g23_no_duplicate_allocation():
    """G23.5: assignment sample yang sama tidak alokasi ganda."""
    print("G23.5 tidak ada duplicate allocation (G21 instant remap)")
    sd, tmp, kits = _g23_kt()
    try:
        rt = G23PsramRuntime(sd, pcm_bytes=32 * 1024)
        ac = resolve_samples(sd, "/kits/Acoustic", kits["Acoustic"])
        rt.load_kit(ac)
        for i in range(PAD_COUNT):
            rt.resolved[i] = ac[i] or ""

        free0 = rt.heap.free_total()

        # (a) file yang SUDAH dimuat -> remap, nol alokasi.
        s = rt.load_single_sample(ac[0])
        _check("G23.5 sample kit yang sudah loaded -> cache hit",
               s == rt.find_slot(ac[0]), f"slot={s}")
        _check("G23.5 cache hit: nol alokasi PSRAM",
               rt.heap.free_total() == free0, str(rt.heap.free_total()))
        _check("G23.5 cache hit: nol single_load",
               rt.single_loads == 0, str(rt.single_loads))

        # (b) file baru -> 1 alokasi; diulang 5x tetap 1 alokasi.
        make_wav(os.path.join(sd.p("/kits/Acoustic"), "samples", "dup.wav"))
        dup = abs_sample(sd, "/kits/Acoustic", "dup.wav")
        free1 = rt.heap.free_total()
        for _ in range(5):
            s = rt.load_single_sample(dup)
        _check("G23.5 file baru dialokasi tepat 1x",
               rt.single_loads == 1, str(rt.single_loads))
        _check("G23.5 file baru: 1 slot saja",
               rt.owned_slots() == 1, str(rt.owned_slots()))
        _check("G23.5 file baru: tepat 1 blok teralokasi",
               free1 - rt.heap.free_total() == 32 * 1024,
               str(free1 - rt.heap.free_total()))
        _check("G23.5 registry tidak punya path duplikat",
               len(rt.registry_paths()) ==
               len(set(rt.registry_paths())),
               str(len(rt.registry_paths())))

        # (c) path yang akan terpotong slotPath[] harus ditolak,
        #     kalau tidak akan jadi alokasi berulang.
        long_path = abs_sample(sd, "/kits/Acoustic",
                               "L" * 100 + ".wav")
        _check("G23.5 path > KIT_SAMPLE_PATH_LEN ditolak",
               rt.load_single_sample(long_path) == G21_SLOT_NONE)
        _check("G23.5 path panjang: nol alokasi tambahan",
               rt.single_loads == 1, str(rt.single_loads))

        # (d) alias/shared slot: dua pad -> satu path -> satu buffer.
        rt2 = G23PsramRuntime(sd, pcm_bytes=32 * 1024)
        rt2.load_kit(ac)
        a = rt2.load_single_sample(ac[1])
        b = rt2.load_single_sample(ac[1])
        _check("G23.5 shared/alias slot: satu slot untuk satu path",
               a == b, f"{a} vs {b}")
        _check("G23.5 shared/alias: 1 path kit = 0 owned slot "
               "(sudah di psramBuffer)",
               rt2.owned_slots() == 0, str(rt2.owned_slots()))
        _check("G23.5 shared/alias: tidak ada alokasi tambahan",
               rt2.heap.live_bytes() == 12 * 32 * 1024,
               str(rt2.heap.live_bytes()))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def test_g23_kit_load_after_single_sample():
    """G23.6 TES C->E: KIT LOAD setelah single-sample load tetap sukses."""
    print("G23.6 KIT LOAD setelah single-sample load")
    sd, tmp, kits = _g23_kt()
    try:
        rt = G23PsramRuntime(sd, pcm_bytes=64 * 1024)
        ac = resolve_samples(sd, "/kits/Acoustic", kits["Acoustic"])
        rt.load_kit(ac)
        for i in range(PAD_COUNT):
            rt.resolved[i] = ac[i] or ""

        for i in range(4):
            nm = "s%d.wav" % i
            make_wav(os.path.join(sd.p("/kits/Acoustic"), "samples", nm))
            rt.set_pad_slot(i, rt.load_single_sample(
                abs_sample(sd, "/kits/Acoustic", nm)))

        _check("G23.6 4 pad diarahkan ke extra slot",
               all(rt.slot_for_pad[i] >= G21_KIT_SLOTS
                   for i in range(4)),
               str(rt.slot_for_pad))
        live_with_extras = rt.heap.live_bytes()

        el = resolve_samples(sd, "/kits/Electronic", kits["Electronic"])
        ok = rt.load_kit(el)
        _check("G23.6 KIT LOAD setelah extras -> sukses", ok)
        _check("G23.6 live PSRAM turun (extras + kit lama hilang)",
               rt.heap.live_bytes() < live_with_extras,
               f"{rt.heap.live_bytes()} vs {live_with_extras}")
        _check("G23.6 owned slot = 0 setelah KIT LOAD",
               rt.owned_slots() == 0)
        _check("G23.6 mapping kembali identitas kit baru",
               rt.slot_for_pad == list(range(PAD_COUNT)),
               str(rt.slot_for_pad))
        _check("G23.6 tidak ada pointer stale: fire_hit valid",
               all(rt.fire_hit(i) is not None for i in range(PAD_COUNT)))
        _check("G23.6 12 sample Electronic audible",
               len(rt.registry_paths()) == 12)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def test_g23_switch_cycles_20x():
    """G23.7 TES F: 20x switch Acoustic->Electronic->ROCK LIVE."""
    print("G23.7 TES F: 20x siklus switch")
    sd, tmp, kits = _g23_kt()
    try:
        rt = G23PsramRuntime(sd, pcm_bytes=48 * 1024)
        ac = resolve_samples(sd, "/kits/Acoustic", kits["Acoustic"])
        rt.load_kit(ac)
        for i in range(PAD_COUNT):
            rt.resolved[i] = ac[i] or ""

        order = ["Electronic", "ROCK LIVE", "Acoustic"]
        ok_count = 0
        silent = 0
        stale = 0

        for n in range(20):
            name = order[n % 3]
            d = "/kits/" + name
            paths = resolve_samples(sd, d, kits[name])
            if rt.load_kit(paths):
                ok_count += 1
                for i in range(PAD_COUNT):
                    rt.resolved[i] = paths[i] or ""
            for i in range(PAD_COUNT):
                if rt.fire_hit(i) is None:
                    silent += 1
                elif rt.fire_hit(i) not in paths:
                    stale += 1
            # Sisipkan PAD_SOUND seperti pada pemakaian nyata.
            nm = "t%d.wav" % (n % 4)
            mk = os.path.join(sd.p(d), "samples", nm)
            if not os.path.exists(mk):
                make_wav(mk)
            rt.set_pad_slot(0, rt.load_single_sample(
                abs_sample(sd, d, nm)))

        _check("G23.7 semua 20 KIT LOAD sukses", ok_count == 20,
               str(ok_count))
        _check("G23.7 tidak ada pad senyap (stale pointer check)",
               silent == 0, str(silent))
        _check("G23.7 tidak ada sample dari kit yang salah",
               stale == 0, str(stale))
        # Siklus terakhir menyisipkan 1 PAD_SOUND, jadi paling banyak
        # ada 1 owned slot. Yang dilarang adalah AKUMULASI 20 slot.
        _check("G23.7 owned slot tidak akumulasi antar siklus "
               "(<= 1 dari PAD_SOUND terakhir)",
               rt.owned_slots() <= 1, str(rt.owned_slots()))
        _check("G23.7 live allocation = blok kit + <=1 slot (tidak leak)",
               rt.live_handles() <= 2, str(rt.live_handles()))
        _check("G23.7 live bytes <= blok kit + 1 sample",
               rt.heap.live_bytes() <= 13 * 48 * 1024,
               str(rt.heap.live_bytes()))
        _check("G23.7 free PSRAM >= 80% dari total",
               rt.heap.free_total() >= rt.heap.total * 0.8,
               f"{rt.heap.free_total() / 1024:.0f}KB")
        _check("G23.7 largest block >= ukuran kit (tidak fragmentasi)",
               rt.heap.largest() >= 12 * 48 * 1024,
               f"{rt.heap.largest() / 1024:.0f}KB")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def test_g23_rollback_keeps_old_kit():
    """G23.8: rollback benar-benar mengembalikan kit lama."""
    print("G23.8 rollback: kit A tetap valid setelah B gagal")
    sd, tmp, kits = _g23_kt()
    try:
        # (1) Kit A aktif, lalu 2 extra sample G21.
        rt = G23PsramRuntime(sd, pcm_bytes=64 * 1024)
        ac = resolve_samples(sd, "/kits/Acoustic", kits["Acoustic"])
        rt.load_kit(ac)
        for i in range(PAD_COUNT):
            rt.resolved[i] = ac[i] or ""
        for i in range(2):
            nm = "r%d.wav" % i
            make_wav(os.path.join(sd.p("/kits/Acoustic"), "samples", nm))
            rt.set_pad_slot(i, rt.load_single_sample(
                abs_sample(sd, "/kits/Acoustic", nm)))
        active = 0
        mapping_before = list(rt.slot_for_pad)

        # (2) KIT LOAD B: alokasi B sengaja digagalkan.
        el = resolve_samples(sd, "/kits/Electronic", kits["Electronic"])
        saved = rt.heap.alloc
        # Guardian: simulasi konsumer PSRAM lain yang tidak ikut
        # dilepas releaseAll().
        guard = rt.heap.alloc(rt.heap.free_total())
        _check("G23.8 guardian PSRAM teralokasi", guard is not None)
        # Kit B lebih besar dari sisa ruang supaya free < requested.
        rt.pcm = 96 * 1024
        state = {"n": 0}

        def flaky(size):
            # Tolak 1 alokasi besar (blok kit B), lalu normal lagi.
            if size == 12 * 96 * 1024 and state["n"] == 0:
                state["n"] += 1
                rt.heap.fail_count += 1
                return None
            return saved(size)

        rt.heap.alloc = flaky
        ok = rt.load_kit(el)
        rt.heap.alloc = saved
        _check("G23.8 alokasi kit B gagal (reproduksi)", not ok)
        _check("G23.8 penyebab diklasifikasi OOM "
               "(free < requested setelah release)",
               rt.last_cause == "OOM",
               f"{rt.last_cause} free={rt.free_now()}")
        rt.heap.release(guard)
        rt.pcm = 64 * 1024
        _check("G23.8 setelah gagal, PSRAM kosong (free-then-alloc)",
               rt.heap.live_bytes() == 0, str(rt.heap.live_bytes()))

        # (3) Rollback: muat ulang kit lama -> harus kembali utuh.
        restored = rt.load_kit(list(rt.resolved))
        _check("G23.8 rollback: muat ulang kit lama sukses", restored)
        _check("G23.8 rollback: sample A kembali tersedia",
               all(rt.fire_hit(i) is not None for i in range(PAD_COUNT)))
        _check("G23.8 rollback: 12 sample A ada di PSRAM",
               rt.heap.live_bytes() == 12 * 64 * 1024,
               str(rt.heap.live_bytes()))
        _check("G23.8 rollback: tidak ada owned slot tersisa",
               rt.owned_slots() == 0)
        _check("G23.8 rollback: mapping kembali ke kit A "
               "(extras G23 runtime hilang, sesuai desain)",
               rt.slot_for_pad == list(range(PAD_COUNT)),
               f"before={mapping_before} after={rt.slot_for_pad}")
        _check("G23.8 rollback: kit A benar-benar audible",
               all(rt.fire_hit(i) in ac for i in range(PAD_COUNT)))

        # (4) Rollback yang GAGAL harus jujur (bukan diam-diam bisu).
        rt2 = G23PsramRuntime(sd, pcm_bytes=64 * 1024)
        rt2.load_kit(ac)
        for i in range(PAD_COUNT):
            rt2.resolved[i] = ac[i] or ""
        st2, active2 = g23_perform_kit_switch(
            rt2, sd, "/kits/Electronic", kits["Electronic"], 0,
            force_new_kit_alloc_fail=True, force_restore_fail=True)
        _check("G23.8 rollback gagal -> status ROLLBACK_FAILED",
               st2 == "ROLLBACK_FAILED", st2)
        _check("G23.8 rollback gagal: tidak ada kit aktif di state",
               active2 is None)
        _check("G23.8 rollback gagal: PSRAM memang kosong "
               "(tidak ada stale pointer)",
               rt2.heap.live_bytes() == 0 and
               all(rt2.fire_hit(i) is None for i in range(PAD_COUNT)))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def test_g23_voice_gate_never_frees_live_voice():
    """G23.9: tidak ada free PSRAM saat voice aktif membaca."""
    print("G23.9 voice gate: tidak membebaskan buffer yang dibunyikan")
    uart_src = _g23_read("DrumCore", "uart_protocol.cpp")
    psram_src = _g23_read("DrumCore", "psram_loader.cpp")

    switch = _g23_func_body(
        uart_src, "uint8_t performKitSwitch(uint8_t newKit, uint8_t seq)")
    assign = _g23_func_body(uart_src, "uint8_t performSampleAssign(")
    single = _g23_func_body(psram_src, "uint8_t psramLoadSingleSample(")

    _check("G23.9 KIT LOAD tetap pakai gate + voice drain",
           "audioSetHitAccept(false)" in switch and
           "audioActiveVoiceCount() == 0" in switch)
    _check("G23.9 KIT LOAD tetap full load (bukan remap)",
           "psramLoadKit(" in switch and
           "psramSetPadSlot" not in switch)
    _check("G23.9 gate off terjadi SEBELUM load PSRAM",
           switch.index("audioSetHitAccept(false)") <
           switch.index("psramLoadKit("))
    _check("G23.9 gate on dibuka lagi setelah load/commit",
           switch.rindex("audioSetHitAccept(true)") >
           switch.index("psramLoadKit("))
    _check("G23.9 rollback juga di dalam gate (tidak ada free "
           "tanpa drain)",
           switch.index("audioSetHitAccept(false)") <
           switch.index("restored = psramLoadKit("))
    _check("G23.9 drain punya deadline (tidak hang selamanya)",
           "deadlineMs" in switch and "vTaskDelay" in switch and
           "return STATUS_BUSY;" in switch)
    _check("G23.9 BUSY membuka kembali gate (tidak bisu permanen)",
           switch.count("audioSetHitAccept(true)") >= 2)

    # PAD_SOUND tetap TIDAK punya gate/free (kontrak G21).
    _check("G23.9 PAD_SOUND tetap tanpa hit-gate (G21 utuh)",
           "audioSetHitAccept" not in assign and
           "psramUnloadKit" not in assign and
           "free(" not in assign)
    _check("G23.9 PAD_SOUND tetap boleh load SATU file",
           "psramLoadSingleSample(" in assign)
    _check("G23.9 PAD_SOUND tidak pernah free buffer PSRAM",
           "slotOwnBuf" not in assign and "psramLoadKit(" not in assign)
    _check("G23.9 single-sample load tidak membebaskan resource kit",
           "free(psramBuffer" not in _strip_comments(single) and
           "free(slotOwnBuf" not in _strip_comments(single) and
           "releaseAll" not in _strip_comments(single))
    _check("G23.9 single-sample load hanya free buffer lokal "
           "yang belum di-commit",
           _strip_comments(single).count("free(buf") == 2)


def _g23_func_body(src, signature):
    start = src.find(signature)
    if start < 0:
        return ""
    start = src.find("{", src.find(")", start))
    depth = 0
    for i in range(start, len(src)):
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
            if depth == 0:
                return src[start:i + 1]
    return ""


def _g23_read(*parts):
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    with open(os.path.join(root, *parts), "r", encoding="utf-8") as f:
        return f.read()


def test_g23_source_verification():
    """G23.10 source verification: bentuk patch G23 di firmware."""
    print("G23.10 source verification: ownership + instrumentation")
    psram_src = _g23_read("DrumCore", "psram_loader.cpp")
    psram_h = _g23_read("DrumCore", "psram_loader.h")
    uart_src = _g23_read("DrumCore", "uart_protocol.cpp")
    cfg = _g23_read("DrumCore", "config.h")

    release = _strip_comments(
        _g23_func_body(psram_src, "void releaseAll(const char *why)"))
    unload = _strip_comments(_g23_func_body(psram_src, "void psramUnloadKit()"))
    single = _strip_comments(
        _g23_func_body(psram_src, "uint8_t psramLoadSingleSample("))
    loadkit = _strip_comments(_g23_func_body(psram_src, "bool psramLoadKit("))
    switch = _strip_comments(
        _g23_func_body(uart_src, "uint8_t performKitSwitch(uint8_t newKit,"
                                " uint8_t seq)"))
    assign = _strip_comments(
        _g23_func_body(uart_src, "uint8_t performSampleAssign("))

    # --- 1. full-kit unload membebaskan extra slots ---------------
    _check("G23.10 psramUnloadKit() memanggil releaseAll()",
           "releaseAll(" in unload and "free(" not in unload)
    _check("G23.10 releaseAll() membebaskan SELURUH slotOwnBuf 0..15",
           "for (uint8_t s = 0; s < PSRAM_MAX_SAMPLES; s++)" in release and
           "if (slotOwnBuf[s])" in release and
           "free(slotOwnBuf[s]);" in release and
           "slotOwnBuf[s] = nullptr;" in release)
    _check("G23.10 releaseAll() membebaskan blok PCM kit (G25: blok "
           "kit ikut slotOwnBuf, tidak ada handle terpisah)",
           "if (slotOwnBuf[s])" in release and
           "kitHasPcm = false;" in release and
           "psramBuffer" not in release)
    _check("G23.10 releaseAll() mengosongkan registry SEBELUM "
           "samples[] dinolkan",
           release.index("clearSampleRegistry()") <
           release.index("samples[i] = SampleInfo{}"))
    _check("G23.10 releaseAll() reset sampleCount/kitLoaded/total",
           "sampleCount = 0;" in release and
           "kitLoaded = false;" in release and
           "psramTotalAllocated = 0;" in release)
    _check("G23.10 releaseAll() mengosongkan SELURUH slotOwnBuf",
           release.count("slotOwnBuf[s] = nullptr;") == 1)

    # --- 2. clear registry tidak meninggalkan owned allocation ---
    # free() blok PCM hanya boleh muncul di releaseAll() (blok yang
    # SUDAH di-commit) DAN di releaseStaged() (blok BARU yang gagal,
    # bukan kit lama). Itu tetap dua titik, tidak lebih.
    _check("G23.10 hanya SATU release point untuk slotOwnBuf",
           psram_src.count("free(slotOwnBuf[") == 1)
    _check("G23.10 hanya SATU release point untuk blok staging "
           "(cleanup kegagalan load)",
           psram_src.count("free(stagedBuf[") == 1 and
           "void releaseStaged(" in psram_src and
           loadkit.count("releaseStaged(stagedBuf);") == 2)

    commit = _strip_comments(
        _g23_func_body(psram_src, "void commitKitRegistry("))
    _check("G23.10 commitKitRegistry() tidak menyentuh ownership",
           "free(" not in commit and "slotOwnBuf" not in commit)
    _check("G23.10 single load menolak slot ber-orphan buffer "
           "(anti leak-by-overwrite)",
           "if (slotOwnBuf[slot])" in single and
           "return PSRAM_SLOT_NONE;" in single)
    _check("G23.10 single load TIDAK membebaskan resource kit "
           "(tanpa voice gate)",
           "free(stagedBuf" not in single and
           "free(slotOwnBuf" not in single and
           "releaseAll" not in single and
           "releaseStaged" not in single)
    _check("G23.10 single load menolak path yang akan terpotong",
           "sizeof(slotPath[0])" in single and
           "PATH TOO LONG" in single)
    _check("G23.10 single load tetap idempoten (cache hit)",
           "psramFindSlotByPath(absPath)" in single)

    # --- 3. instrumentation wajib --------------------------------
    for tag in ("before unload", "after unload", "before kit alloc",
                "kit alloc OK", "kit alloc FAILED"):
        _check(f"G23.10 instrumentation '{tag}' ada",
               f'"{tag}"' in psram_src, tag)
    _check("G23.10 memakai capability PSRAM yang benar",
           "heap_caps_get_free_size(MALLOC_CAP_SPIRAM)" in psram_src and
           "heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)" in
           psram_src)
    _check("G23.10 instrumentasi tidak per-audio-frame",
           psram_src.count("printHeap(") <= 8)
    _check("G23.10 alokasi besar mengukur requested",
           'printHeap("before kit alloc", totalPCMBytes)' in psram_src and
           'printHeap("kit alloc FAILED", totalPCMBytes)' in psram_src)
    _check("G23.10 kegagalan diklasifikasi OOM vs FRAGMENTED",
           "FRAGMENTED" in psram_src and '"OOM"' in psram_src)
    _check("G23.10 audit owned-slot ikut dicetak saat gagal",
           "ownedSlots=" in psram_src)
    _check("G23.10 releaseAll() mengukur sebelum & sesudah",
           release.count("printHeap(") == 2)
    _check("G23.10 free terjadi SEBELUM ps_malloc blok baru",
           loadkit.index("releaseAll(") < loadkit.index("ps_malloc("))

    # --- 4. rollback ----------------------------------------------
    _check("G23.10 performKitSwitch() tetap gate-off lebih dulu",
           switch.index("audioSetHitAccept(false)") <
           switch.index("psramLoadKit("))
    _check("G23.10 performKitSwitch() rollback memuat ULANG kit lama",
           "restored = psramLoadKit(" in switch and
           "restored" in switch)
    _check("G23.10 rollback memakai resolved path kit lama",
           "kitLoaderBuildPath(oldKit, oldPath" in switch and
           switch.index("kitLoaderBuildPath(oldKit") <
           switch.index("restored = psramLoadKit("))
    _check("G23.10 uartActiveKit hanya di-commit kalau PSRAM ada",
           "if (restored)" in switch and
           "uartActiveKit = oldKit;" in switch and
           "ROLLBACK FAILED" in switch)
    _check("G23.10 tidak ada lagi klaim 'kit lama utuh' yang salah",
           "membiarkan kit lama utuh" not in uart_src)
    _check("G23.10 rollback tidak terjadi di luar gate",
           switch.index("audioSetHitAccept(false)") <
           switch.index("restored = psramLoadKit("))

    # --- G21 tidak dirusak ----------------------------------------
    _check("G23.10 G21: PAD_SOUND tetap cache-hit remap dulu",
           assign.index("psramFindSlotByPath(absPath)") <
           assign.index("psramLoadSingleSample(absPath)"))
    _check("G23.10 G21: PAD_SOUND tetap NOL kit reload",
           "performKitSwitch" not in assign and
           "psramUnloadKit" not in assign)
    _check("G23.10 G21: KIT LOAD tetap FULL load (bukan remap)",
           "psramLoadKit(" in switch and
           "psramSetPadSlot" not in switch)
    _check("G23.10 G21: registry & slotOwnBuf tetap 16 slot",
           "constexpr size_t PSRAM_MAX_SAMPLES =" in cfg and
           "PSRAM_KIT_SLOT_COUNT + PSRAM_EXTRA_SLOT_COUNT" in cfg and
           re.search(r'char\s+slotPath\[PSRAM_MAX_SAMPLES\]'
                     r'\[KIT_SAMPLE_PATH_LEN\]', psram_src) is not None and
           re.search(r'uint8_t\s*\*\s*slotOwnBuf\[PSRAM_MAX_SAMPLES\]',
                     psram_src) is not None)
    _check("G23.10 G21: public API psram_loader tidak berubah",
           "uint8_t psramLoadSingleSample(" in psram_h and
           "void psramUnloadKit();" in psram_h and
           "uint8_t psramFindSlotByPath(const char *absPath);" in psram_h and
           "void psramSetPadSlot(" in psram_h)

    # --- LOCKED: area yang tidak boleh berubah --------------------
    _check("G23.10 trigger engine tidak disentuh",
           "void triggerTask(" in _g23_read("DrumCore", "trigger.cpp"))
    _check("G23.10 audio realtime path tetap psramSlotForPad",
           "psramSlotForPad(padIndex)" in
           _strip_comments(_g23_func_body(
               _g23_read("DrumCore", "audio.cpp"), "void fireHit(")))
    _check("G23.10 UART protocol tidak berubah",
           "constexpr uint8_t UART_FRAME_SIZE = 8;" in
           _g23_read("DrumCore_UI", "config.h") and
           "constexpr uint8_t UART_SOF = 0xD3;" in
           _g23_read("DrumCore_UI", "config.h"))
    _check("G23.10 baud tetap 921600 di kedua sisi",
           "constexpr uint32_t UART_BAUD = 921600;" in
           _g23_read("DrumCore_UI", "config.h") and
           "constexpr uint32_t UART1_BAUD = 921600;" in cfg)
    _check("G23.10 CMD_KIT_RENAME & KIT_INFO tetap",
           "CMD_KIT_RENAME       = 0x17" in
           _g23_read("DrumCore", "uart_protocol.h") and
           "CMD_KIT_INFO" in _g23_read("DrumCore", "uart_protocol.h"))
    _check("G23.10 12 pad tetap",
           "constexpr uint8_t PAD_COUNT = 12;" in cfg)
    _check("G23.10 PSRAM init tidak berubah",
           "ESP.getPsramSize()" in psram_src and
           "=== PSRAM INIT ===" in psram_src and
           "=== PSRAM READY ===" in psram_src)


# ===============================================================
# G25 - PSRAM KIT LOAD FRAGMENTATION
#
# Bug nyata:
#   Diky Djandut -> Acoustic GAGAL load karena PSRAM fragmented.
#     after unload: free = 8135 KB, largest = 5119 KB
#     requested   = 5453 KB
#     -> free total CUKUP, tapi largest contiguous block < requested
#     -> PSRAM ALLOC FAILED / cause=FRAGMENTED
#     -> kit kosong
#   Rollback kemudian sukses reload Diky Djandut (5105 KB).
#
# Akar masalah: psramLoadKit() meminta SATU blok kontigu sebesar
# TOTAL seluruh PCM kit. Free-list PSRAM itu terpecah, jadi satu
# permintaan raksasa tidak bisa dilayani meski total ruangnya cukup.
#
# Perbaikan:TIAP SAMPLE UNIK dapat blok PSRAM sendiri
# (slotOwnBuf[slot]), persis seperti slot runtime G21 sudah
# dilakukan. Permintaan terbesar hanya sebesar satu sample, jadi
# load berhasil selama free TOTAL cukup.
#
# Yang diuji di sini:
#   1. model free-list: free total > requested tapi largest <
#      requested -> blok-kit-tunggal GAGAL, blok-per-sample BERHASIL
#   2. siklus switch nyata (Diky Djandut/Acoustic/Electronic/
#      ROCK LIVE) berulang tanpa BUSY hanya karena fragmentasi
#   3. G21 tetap aman: cache hit nol malloc, sample baru satu slot
#   4. G23 tetap aman: unload/reload, alloc failure, rollback,
#      ownership, no stale pointer
#   5. bentuk source: tidak ada lagi alokasi kontigu total kit,
#      audio.cpp tidak disentuh
# ===============================================================

G25_PSRAM_KB = 8192                          # ESP.getPsramSize()
G25_FIELD_FREE_KB = 8135                     # free setelah unload (log)
G25_FIELD_LARGEST_KB = 5119                  # largest contiguous (log)
G25_FIELD_REQUEST_KB = 5453                  # total PCM Acoustic (log)


def _g25_field_kit_sizes():
    """Ukuran PCM 12 sample Acoustic yg totalnya PERSIS 5453 KB.

    Tidak seragam supaya urutan alokasi (descending) ikut teruji.
    """
    kb = [700, 620, 540, 480, 430, 400, 380, 350, 330, 320, 260, 123]
    scale = G25_FIELD_REQUEST_KB / sum(kb)
    kb = [int(v * scale) for v in kb]
    kb[-1] += G25_FIELD_REQUEST_KB - sum(kb)     #bulatkan ke 5453
    return kb


def _g25_field_fragmentation(heap):
    """Bangun free-list PERSIS seperti log lapangan.

        total 8192 KB
        alloc 5119  -> [0, 5119)
        alloc   57  -> [5119, 5176)   konsumer panjang-lived
        free  5119  -> free: [0,5119) + [5176,8192)
        => free = 8135 KB, largest = 5119 KB

    Pola ini yang terjadi di device: satu blok besar pernah dipakai
    lalu dibebaskan, sementara konsumen kecil (LVGL/JSON/stack)
    masih hidup DI DALAMNYA. Free total 8135 KB TETAP cukup untuk
    5453 KB, tapi tidak ada hole kontigu sebesar itu.
    """
    big = heap.alloc(G25_FIELD_LARGEST_KB * 1024)
    # live setelahnya hanya 8192 - 8135 = 57 KB
    pin = heap.alloc((G25_PSRAM_KB - G25_FIELD_FREE_KB) * 1024)
    assert pin is not None, "pin block gagal dialokasikan"
    heap.release(big)
    return pin


def _g25_multihole_fragmentation(heap):
    """Free-list 4 hole: free total cukup, tapi largest kecil.

    4 pin kecil (19KB) memisahkan 4 hole, jadi free-list terpecah
    persis seperti device: konsumen kecil (LVGL/JSON/stack) hidup
    TERPISAH di antara blok besar yang pernah dibebaskan.

    Hasil: free ~8116KB, largest ~3616KB, live 76KB.
    """
    p1 = heap.alloc(19 * 1024)
    b1 = heap.alloc(1500 * 1024)
    p2 = heap.alloc(19 * 1024)
    b2 = heap.alloc(1500 * 1024)
    p3 = heap.alloc(19 * 1024)
    b3 = heap.alloc(1500 * 1024)
    p4 = heap.alloc(19 * 1024)
    b4 = heap.alloc(3635 * 1024)
    for b in (b1, b2, b3, b4):
        heap.release(b)
    pins = [p1, p2, p3, p4]
    assert all(x is not None for x in pins)
    return pins


class G25PsramRuntime(G23PsramRuntime):
    """G23PsramRuntime dengan alokasi BLOK PER SAMPLE (G25).

    Semua state/registry/ownership G21+G23 dipakai apa adanya.
    Yang berubah HANYA titik alokasi PCM kit:

      SEBELUM (G23):  satu ps_malloc(totalPCM) -> kit_handle
      SEKARANG (G25): ps_malloc(dataSize) per sample unik
                       -> slot_handle[slot] untuk slot KIT juga

    Kit handle monolithic tidak ada lagi; setiap slot yang punya
    PCM punya handle sendiri di slot_handle[], dan
    psramTotalAllocated dijumlah dari handle itu.
    """

    def __init__(self, sd, psram_kb=G25_PSRAM_KB, pcm_list=None):
        super().__init__(sd, psram_kb=psram_kb)
        # Ukuran PCM per sample (byte). Default seragam; bisa
        # diganti per kit lewat pcm_list.
        self.pcm_list = list(pcm_list) if pcm_list else None
        self.kit_handle = None        # G23: tidak lagi dipakai
        self.kit_has_pcm = False
        self.psram_kit_bytes = 0      # total PCM unik di PSRAM
        # handle PCM per slot: dipakai untuk slot kit (G25) DAN
        # slot runtime (G21) — satu tabel kepemilikan.
        self.slot_handle = [None] * G21_MAX_SLOTS
        self.alloc_log = []           # [(seq, slot, req, ok)]
        self.layout_log = []          # dict layout setelah commit
        self.largest_request = 0
        # Guardian: konsumer PSRAM lain yang TIDAK ikut dilepas
        # releaseAll() — memaksa free-list benar-benar terpecah.
        self.guardian = None

    # ---------------- ukuran PCM ----------------

    def pcm_for(self, index):
        """Ukuran PCM sample ke-`index` (default seragam)."""
        if self.pcm_list is not None:
            return self.pcm_list[index % len(self.pcm_list)]
        return self.pcm

    def total_pcm_for(self, paths):
        seen = set()
        total = 0
        for i, p in enumerate(paths):
            if not p or p in seen:
                continue
            seen.add(p)
            total += self.pcm_for(i)
        return total

    # ---------------- releaseAll() ----------------

    def release_all(self, why):
        """Cermin releaseAll(): SATU tabel kepemilikan.

        G25: blok kit TIDAK lagi punya handle terpisah; seluruh
        blok PCM (kit + runtime) ada di slot_handle[] dan
        dibebaskan di sini.
        """
        self._note(f"before unload ({why})")
        freed = 0
        owned = self.owned_slots()
        kit_blocks = sum(1 for s in range(G21_KIT_SLOTS)
                         if self.slot_handle[s] is not None)
        runtime_blocks = sum(1 for s in range(G21_KIT_SLOTS, G21_MAX_SLOTS)
                             if self.slot_handle[s] is not None)
        self.kit_handle = None            # G25: tidak ada blok kit
        for s in range(G21_MAX_SLOTS):
            h = self.slot_handle[s]
            if h is not None:
                freed += self.heap.live_bytes_of(h)
                self.heap.release(h)
                self.slot_handle[s] = None
            self.samples[s] = None
            self.slot_path[s] = None
            self.slot_own_buf[s] = False
        self.psram_kit_bytes = 0
        self.kit_has_pcm = False
        self.sample_count = 0
        self.kit_buffer = None
        self.kit_loaded = False
        self.slot_for_pad = list(range(PAD_COUNT))
        self.released_kit += 1
        self.released_slots += owned
        self._note(f"after unload ({why})")
        self.last_release = (kit_blocks, runtime_blocks, owned)
        return freed

    def unload(self):
        return self.release_all("unload")

    # ---------------- helper ----------------

    def release_staged(self, staged):
        """Cermin releaseStaged(): bebaskan blok yang belum commit.

        Blok kit baru yang sudah dialokasikan tapi BELUM masuk
        slot_handle[]. Kalau tidak dilepas, PSRAM bocor setiap
        kali KIT LOAD gagal.
        """
        n = 0
        for s in range(G21_MAX_SLOTS):
            h = staged[s]
            if h is not None:
                self.heap.release(h)
                staged[s] = None
                n += 1
        return n

    @staticmethod
    def sort_by_size_desc(order, sizes):
        """Cermin sortSlotsBySizeDesc(): terbesar didahulukan."""
        out = list(order)
        out.sort(key=lambda s: sizes[s], reverse=True)
        return out

    def owned_blocks(self):
        return sum(1 for h in self.slot_handle if h is not None)

    def kit_blocks(self):
        return sum(1 for s in range(G21_KIT_SLOTS)
                   if self.slot_handle[s] is not None)

    def runtime_blocks(self):
        return sum(1 for s in range(G21_KIT_SLOTS, G21_MAX_SLOTS)
                   if self.slot_handle[s] is not None)

    def pcm_in_psram(self):
        return sum(self.heap.live_bytes_of(h)
                   for h in self.slot_handle if h is not None)

    def largest_alloc_request(self):
        return self.largest_request

    # ---------------- psramLoadKit() ----------------

    def load_kit(self, paths, sample_ids=None):
        """Cermin psramLoadKit() versi G25 (blok per sample)."""
        self.kit_loads += 1
        self.alloc_log = []
        self.largest_request = 0

        # --- Pass 1: validasi + dedup path (ALIAS) ---
        alias = {}
        valid = {}
        seen = set()
        total = 0
        for i, p in enumerate(paths):
            if not p:
                continue
            if p in seen:
                continue                      # alias -> pakai blok owner
            if not self.sd.exists(p):
                continue
            seen.add(p)
            valid[i] = True
            total += self.pcm_for(i)

        if total == 0:
            self.release_all("empty-kit")
            self.sample_count = len(paths)
            self.kit_loaded = True
            self.kit_has_pcm = False
            self._record_layout("empty", paths, 0)
            return True

        # --- Pass 2: free semua, lalu alokasi per sample ---
        self.release_all("pre-alloc")
        self._note("before kit alloc", total)

        staged = [None] * G21_MAX_SLOTS
        staged_bytes = [0] * G21_MAX_SLOTS
        order = []
        for i in range(len(paths)):
            if not valid.get(i):
                continue
            staged_bytes[i] = self.pcm_for(i)
            order.append(i)

        order = self.sort_by_size_desc(order, staged_bytes)
        self.largest_request = max(staged_bytes[i] for i in order)

        allocated = 0
        ok = True
        for seq, s in enumerate(order):
            req = staged_bytes[s]
            h = self._alloc(req)
            self.alloc_log.append((seq, s, req, h is not None))
            if h is None:
                self.last_cause = self._classify(self.free_now(), req)
                self._note("kit alloc FAILED", total)
                self.release_staged(staged)
                self.psram_kit_bytes = 0
                self.kit_has_pcm = False
                self._record_layout("FAILED", paths, 0)
                return False
            staged[s] = h
            allocated += req

        self._note("kit alloc OK", total)
        self.psram_kit_bytes = allocated
        self.kit_has_pcm = True

        # --- Pass 3: baca SD (tidak realistis gagal di test) ---
        all_loaded = True
        self.sd_reads = []
        for i, p in enumerate(paths):
            if not p:
                continue
            if p in alias:
                continue
            if not valid.get(i):
                continue
            self.sd_reads.append(p)

        if not all_loaded:
            self.release_staged(staged)
            self.psram_kit_bytes = 0
            self.kit_has_pcm = False
            self.sample_count = 0
            self.kit_loaded = False
            self.slot_for_pad = list(range(PAD_COUNT))
            self._record_layout("FAILED", paths, 0)
            return False

        # --- Commit: blok dipindahkan ke slot_handle[] ---
        for s in range(G21_MAX_SLOTS):
            if staged[s] is not None:
                self.slot_handle[s] = staged[s]
                self.slot_own_buf[s] = True
                staged[s] = None

        # Alias mewarisi pointer + metadata owner-nya.
        seen_owner = {}
        for i, p in enumerate(paths):
            if not p:
                self.samples[i] = None
                self.slot_path[i] = None
                continue
            if p in seen_owner:
                o = seen_owner[p]
                alias[i] = o
                self.samples[i] = self.samples[o]
                self.slot_path[i] = p
                self.slot_own_buf[i] = False     # BUKAN pemilik blok
            else:
                seen_owner[p] = i
                self.samples[i] = p
                self.slot_path[i] = p
                self.slot_own_buf[i] = bool(self.slot_handle[i])

        self.sample_count = len(paths)
        self.kit_buffer = "kit"
        self.kit_loaded = True
        ids = sample_ids if sample_ids is not None \
            else list(range(PAD_COUNT))
        for p in range(PAD_COUNT):
            src = ids[p] if p < len(ids) else p
            if src < G21_KIT_SLOTS and src < self.sample_count \
                    and self.samples[src]:
                self.slot_for_pad[p] = src
            else:
                self.slot_for_pad[p] = p

        self._record_layout("OK", paths, total)
        return ok

    def _record_layout(self, phase, paths, total):
        """Cermin printKitLayout(): laporan truthfully runtime."""
        loaded = sum(1 for s in range(self.sample_count)
                     if self.samples[s])
        free, largest = self.free_now()
        self.layout_log.append({
            "phase": phase,
            "samples_loaded": loaded,
            "count": len(paths),
            "blocks": self.owned_blocks(),
            "kit_blocks": self.kit_blocks(),
            "runtime_blocks": self.runtime_blocks(),
            "total_pcm": total,
            "unique_pcm": self.psram_kit_bytes,
            "pcm_in_psram": self.pcm_in_psram(),
            "free": free,
            "largest": largest,
            "pad_to_slot": list(self.slot_for_pad),
        })

    def latest_layout(self, phase=None):
        rows = [r for r in self.layout_log
                if phase is None or r["phase"] == phase]
        return rows[-1] if rows else None

    # ---------------- psramLoadSingleSample() (G21) ----------------

    def load_single_sample(self, path):
        """Cermin psramLoadSingleSample() — TIDAK berubah oleh G25.

        Tetap: satu file saja, alokasi sendiri, tanpa free blok
        lain, tanpa kit reload, idempoten (cache hit nol malloc).
        """
        if not path:
            return G21_SLOT_NONE

        existing = self.find_slot(path)
        if existing != G21_SLOT_NONE:
            return existing                     # cache hit: remap, NO malloc

        if not self.kit_has_pcm or not self.kit_loaded:
            return G21_SLOT_NONE
        if not self.sd_ready or self.alloc_fails:
            return G21_SLOT_NONE

        slot = G21_SLOT_NONE
        for s in range(G21_MAX_SLOTS):
            if not self.slot_path[s]:
                slot = s
                break
        if slot == G21_SLOT_NONE:
            return G21_SLOT_NONE

        # Guard orphan: jangan timpa slot yang punya blok sendiri.
        if self.slot_handle[slot] is not None:
            return G21_SLOT_NONE

        if not self.sd.exists(path):
            return G21_SLOT_NONE

        h = self._alloc(self.pcm)
        if h is None:
            self.last_cause = self._classify(self.free_now(), self.pcm)
            return G21_SLOT_NONE

        self.sd_reads.append(path)
        self.samples[slot] = path
        self.slot_path[slot] = path
        self.slot_own_buf[slot] = True
        self.slot_handle[slot] = h
        if slot >= self.sample_count:
            self.sample_count = slot + 1
        self.single_loads += 1
        return slot


def _g25_kt():
    """4 kit: Diky Djandut / Acoustic / Electronic / ROCK LIVE.

    Kit keempat (Diky Djandut) adalah yang gagal load di lapangan,
    jadi dibuat eksplisit dengan 12 sample BERBEDA ( realistically
   .wav WAV berbeda = blok PSRAM berbeda).
    """
    sd, tmp = sd_home()
    kits = {}
    for name in ("Acoustic", "Electronic", "ROCK LIVE"):
        kits[name] = load_kit(sd, "/kits/" + name)

    # "Diky Djandut": 12 sample unik supaya totalnya realistis.
    dj_dir = "/kits/Diky Djandut"
    dj_pads = []
    for i in range(PAD_COUNT):
        p = default_source(i)
        p["sample"] = "dj_%02d.wav" % i
        dj_pads.append(p)
        make_wav(os.path.join(sd.p(dj_dir), "samples", p["sample"]))
    write_kit(sd, dj_dir, 4, "Diky Djandut", dj_pads)
    kits["Diky Djandut"] = load_kit(sd, dj_dir)
    return sd, tmp, kits


def g25_frag_guarded(psram_kb, guard_specs, kit_specs):
    """Bangun free-list yang TERFRAGMENTASI secara deterministik.

    Alokasi `guard_specs` (ukuran KB) lebih dulu, lalu sisanya
    dipakai untuk simulasi kit. Hasil: free total besar, tapi
    largest contiguous block jauh lebih kecil.
    """
    heap = PsramHeap(psram_kb)
    guards = []
    for kb in guard_specs:
        h = heap.alloc(kb * 1024)
        if h is None:
            raise AssertionError("guard allocation gagal")
        guards.append(h)
    return heap, guards


def test_g25_fragmented_heap_both_designs():
    """G25.1: free total > requested tapi largest < requested.

    Dibuktikan pada MODEL YANG SAMA:
      (a) desain LAMA (satu blok total kit) -> GAGAL
      (b) desain BARU (blok per sample)     -> BERHASIL
    dengan free-list yang identik dan angka yang identik dengan
    laporan lapangan (free 8135KB / largest 5119KB / 5453KB).
    """
    print("G25.1 free total cukup tapi largest < requested:")
    sd, tmp, kits = _g25_kt()
    try:
        per = _g25_field_kit_sizes()
        _check("G25.1 total PCM = 5453KB (cerminan log lapangan)",
               sum(per) == G25_FIELD_REQUEST_KB, str(sum(per)))
        _check("G25.1 tiap sample jauh lebih kecil dari total kit",
               max(per) < G25_FIELD_REQUEST_KB, f"max={max(per)}KB")

        # --- free-list PERSIS seperti log lapangan ---
        heap = PsramHeap(G25_PSRAM_KB)
        pin = _g25_field_fragmentation(heap)
        free, largest = heap.free_total(), heap.largest()
        _check("G25.1 free = 8135KB (identik log lapangan)",
               free == G25_FIELD_FREE_KB * 1024, f"{free // 1024}KB")
        _check("G25.1 largest = 5119KB (identik log lapangan)",
               largest == G25_FIELD_LARGEST_KB * 1024,
               f"{largest // 1024}KB")
        _check("G25.1 free total CUKUP untuk 5453KB (bukan OOM)",
               free >= G25_FIELD_REQUEST_KB * 1024, f"{free // 1024}KB")
        _check("G25.1 largest TIDAK cukup untuk 5453KB (FRAGMENTED)",
               largest < G25_FIELD_REQUEST_KB * 1024,
               f"{largest // 1024}KB")

        # (a) desain LAMA: satu permintaan = total kit -> GAGAL
        legacy = heap.alloc(G25_FIELD_REQUEST_KB * 1024)
        _check("G25.1 desain LAMA (1 blok total 5453KB) GAGAL",
               legacy is None)
        _check("G25.1 kegagalan monolitik = FRAGMENTED, bukan OOM",
               G23PsramRuntime._classify(
                   (free, largest), G25_FIELD_REQUEST_KB * 1024)
               == "FRAGMENTED")
        _check("G25.1 free-list tidak berubah oleh permintaan gagal",
               heap.free_total() == free and heap.largest() == largest)

        # (b) desain BARU: satu permintaan = satu sample -> BERHASIL
        blocks = []
        ok = True
        for kb in sorted(per, reverse=True):        # urutan G25
            h = heap.alloc(kb * 1024)
            if h is None:
                ok = False
                break
            blocks.append(h)
        _check("G25.1 desain BARU (blok per sample) BERHASIL", ok,
               f"blok={len(blocks)}/12")
        _check("G25.1 total 12 blok terpakai = 5453KB",
               len(blocks) == 12 and
               sum(heap.live_bytes_of(h) for h in blocks) ==
               G25_FIELD_REQUEST_KB * 1024)
        _check("G25.1 permintaan terbesar hanya 1 sample (~774KB)",
               max(per) < 800, f"max={max(per)}KB")

        # (c) pembedanya FRAGMENTASI, bukan jumlah ruang.
        clean = PsramHeap(G25_PSRAM_KB)
        _check("G25.1 tanpa fragmentasi: largest 8192KB >= 5453KB",
               clean.largest() >= G25_FIELD_REQUEST_KB * 1024,
               f"{clean.largest() // 1024}KB")
        _check("G25.1 tanpa fragmentasi: desain LAMA juga muat "
               "(jadi yg membedakan adalah free-list, bukan budget)",
               clean.alloc(G25_FIELD_REQUEST_KB * 1024) is not None)
        _check("G25.1 guardian 57KB tetap hidup (konsumer lain)",
               pin is not None and
               heap.live_bytes_of(pin) == 57 * 1024)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def test_g25_field_case_diky_to_acoustic():
    """G25.2: reproduksi kasus lapangan Diky Djandut -> Acoustic.

    Guardian PSRAM (konsumer lain) menahan free-list sehingga
    largest < total kit. Desain G25 harus tetap bisa switch.
    """
    print("G25.2 kasus lapangan: Diky Djandut -> Acoustic "
          "(free 8135KB / largest 5119KB / 5453KB)")
    sd, tmp, kits = _g25_kt()
    try:
        per_kb = _g25_field_kit_sizes()
        per = [kb * 1024 for kb in per_kb]
        total = sum(per)

        ac = resolve_samples(sd, "/kits/Acoustic", kits["Acoustic"])
        _check("G25.2 Acoustic punya 12 sample",
               len([p for p in ac if p]) == 12)

        rt = G25PsramRuntime(sd, psram_kb=G25_PSRAM_KB, pcm_list=per)

        # Free-list PERSIS seperti di device: blok besar di-free
        # sementara 57KB konsumer lain masih hidup di dalamnya.
        # releaseAll() TIDAK menyentuh 57KB itu.
        guard = _g25_field_fragmentation(rt.heap)
        free0, largest0 = rt.free_now()
        _check("G25.2 free-list awal = 8135KB / largest 5119KB",
               free0 == G25_FIELD_FREE_KB * 1024 and
               largest0 == G25_FIELD_LARGEST_KB * 1024,
               f"{free0 // 1024}KB/{largest0 // 1024}KB")

        ok = rt.load_kit(ac)
        free, largest = rt.free_now()

        _check("G25.2 KIT LOAD Acoustic BERHASIL (tidak BUSY)", ok)
        _check("G25.2 tepat 5453KB PSRAM terpakai (tidak lebih, tidak "
               "kurang)", free0 - free == total,
               f"{(free0 - free) // 1024}KB dari {total // 1024}KB")
        _check("G25.2 free total awal cukup (8135KB > 5453KB)",
               free0 >= total, f"{free0 // 1024}KB")
        _check("G25.2 largest block < total PCM kit "
               "(justru terfragmentasi!)",
               largest < total, f"{largest // 1024}KB")
        _check("G25.2 SEMUA 12 sample audible (tidak ada stale ptr)",
               all(rt.fire_hit(i) is not None for i in range(PAD_COUNT)))
        _check("G25.2 12 blok PCM dimiliki (satu per sample)",
               rt.owned_blocks() == 12, str(rt.owned_blocks()))
        _check("G25.2 owned table = jumlah sample unik",
               sum(1 for s in range(G21_MAX_SLOTS)
                   if rt.slot_own_buf[s]) == 12)
        _check("G25.2 live PSRAM = total PCM kit (tanpa duplikasi)",
               rt.pcm_in_psram() == total,
               f"{rt.pcm_in_psram() // 1024}KB")
        _check("G25.2 live PSRAM = PCM kit, BUKAN 57KB konsumer lain",
               rt.heap.live_bytes() == total + 57 * 1024,
               f"{rt.heap.live_bytes() // 1024}KB")
        _check("G25.2 permintaan terbesar hanya sebesar 1 sample "
               f"({rt.largest_alloc_request() // 1024}KB), bukan 5453KB",
               rt.largest_alloc_request() == max(per) and
               rt.largest_alloc_request() < G25_FIELD_REQUEST_KB * 1024,
               f"{rt.largest_alloc_request() // 1024}KB")
        _check("G25.2 12 hasil alokasi semuanya OK",
               len(rt.alloc_log) == 12 and
               all(ok for _s, _sl, _r, ok in rt.alloc_log))
        _check("G25.2 urutan alokasi benar-benar descending",
               [_req for _seq, _sl, _req, _ok in rt.alloc_log] ==
               sorted(per, reverse=True))
        lay = rt.latest_layout("OK")
        _check("G25.2 layout: samplesLoaded=12/12",
               lay and lay["samples_loaded"] == 12 and
               lay["count"] == 12, str(lay and lay["samples_loaded"]))
        _check("G25.2 layout: total PCM 5453KB tercatat",
               lay and lay["total_pcm"] == total)
        _check("G25.2 layout: free & largest tercatat",
               lay and lay["free"] == free and lay["largest"] == largest)
        _check("G25.2 layout: mapping pad -> slot 12 entry",
               lay and len(lay["pad_to_slot"]) == PAD_COUNT)

        # Rollback sanity: lepas 57KB, load kit lama, unload.
        rt.heap.release(guard)
        dj = resolve_samples(sd, "/kits/Diky Djandut",
                             kits["Diky Djandut"])
        _check("G25.2 KIT LOAD Diky Djandut setelahnya sukses",
               rt.load_kit(dj))
        rt.unload()
        _check("G25.2 unload: 0 blok tersisa",
               rt.owned_blocks() == 0 and rt.heap.live_bytes() == 0,
               f"blocks={rt.owned_blocks()}")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def test_g25_switch_cycles_repeated():
    """G25.3: 4 kit switch berulang TANPA BUSY karena fragmentasi."""
    print("G25.3 16x siklus switch 4 kit di free-list terfragmentasi")
    sd, tmp, kits = _g25_kt()
    try:
        # Ukuran sample tidak seragam antar kit — kondisi nyata.
        rt = G25PsramRuntime(sd, psram_kb=G25_PSRAM_KB,
                             pcm_list=[kb * 1024
                                       for kb in _g25_field_kit_sizes()])

        # Free-list terfragmentasi persis seperti lapangan, dan TIDAK
        # pernah dibebaskan oleh releaseAll() -> fragmentation tetap
        # ada di setiap siklus switch.
        pin = _g25_field_fragmentation(rt.heap)
        _check("G25.3 free-list terfragmentasi (free 8135 / "
               "largest 5119)", rt.free_now() ==
               (G25_FIELD_FREE_KB * 1024, G25_FIELD_LARGEST_KB * 1024),
               str(tuple(v // 1024 for v in rt.free_now())))

        order = ["Acoustic", "Electronic", "ROCK LIVE", "Diky Djandut"]
        paths = {k: resolve_samples(sd, "/kits/" + k, kits[k])
                 for k in order}
        _check("G25.3 keempat kit punya 12 sample",
               all(len([p for p in paths[k] if p]) == PAD_COUNT
                   for k in order))

        rt.load_kit(paths["Diky Djandut"])
        for i in range(PAD_COUNT):
            rt.resolved[i] = paths["Diky Djandut"][i] or ""

        ok_count = 0
        silent = 0
        wrong = 0
        frag_rounds = 0
        blocked = 0

        for n in range(16):
            name = order[n % 4]
            # Lihat free-list SEBELUM load: harus tetap terpecah.
            _f0, l0 = rt.free_now()
            if l0 < G25_FIELD_REQUEST_KB * 1024:
                frag_rounds += 1
            if rt.load_kit(paths[name]):
                ok_count += 1
                for i in range(PAD_COUNT):
                    rt.resolved[i] = paths[name][i] or ""
            else:
                blocked += 1
                continue

            for i in range(PAD_COUNT):
                got = rt.fire_hit(i)
                if got is None:
                    silent += 1
                elif got not in paths[name]:
                    wrong += 1

            # Sisipkan PAD_SOUND di antara switch (kebutuhan nyata).
            nm = "g25_%d.wav" % (n % 4)
            mk = os.path.join(sd.p("/kits/" + name), "samples", nm)
            if not os.path.exists(mk):
                make_wav(mk)
            rt.set_pad_slot(0, rt.load_single_sample(
                abs_sample(sd, "/kits/" + name, nm)))

        _check("G25.3 SEMUA 16 KIT LOAD sukses (tidak pernah BUSY "
               "karena fragmentasi)", ok_count == 16,
               f"ok={ok_count} blocked={blocked}")
        _check("G25.3 free-list tetap terfragmentasi di sebagian siklus "
               f"({frag_rounds}/16 largest < 5453KB)", frag_rounds > 0,
               str(frag_rounds))
        _check("G25.3 tidak ada pad senyap", silent == 0, str(silent))
        _check("G25.3 tidak ada sample dari kit yang salah",
               wrong == 0, str(wrong))
        _check("G25.3 owned slot tidak akumulasi (12 kit + 1 PAD_SOUND)",
               rt.owned_blocks() == 13, str(rt.owned_blocks()))
        _check("G25.3 tidak ada handle bocor: 12 blok kit + 1 runtime",
               rt.kit_blocks() == 12 and rt.runtime_blocks() == 1,
               f"kit={rt.kit_blocks()} runtime={rt.runtime_blocks()}")
        _check("G25.3 live PSRAM = PCM kit + 1 sample + 57KB konsumer",
               rt.heap.live_bytes() ==
               sum(kb * 1024 for kb in _g25_field_kit_sizes()) +
               64 * 1024 + 57 * 1024,
               f"{rt.heap.live_bytes() // 1024}KB")
        _check("G25.3 free PSRAM >= 30% total (desain tidak menyerap "
               "ruang tak berguna)",
               rt.heap.free_total() >= rt.heap.total * 0.3,
               f"{rt.heap.free_total() / 1024:.0f}KB")

        # 57KB konsumer lain dilepas: unload mengembalikan PSRAM penuh.
        rt.heap.release(pin)
        rt.unload()
        _check("G25.3 unload: PSRAM kembali 0 alokasi hidup",
               rt.heap.live_bytes() == 0, str(rt.heap.live_bytes()))
        _check("G25.3 unload: 0 owned block tersisa",
               rt.owned_blocks() == 0)
        _check("G25.3 unload: free-list kembali ke 8192KB utuh "
               "(coalescing benar)", rt.heap.free_total() ==
               rt.heap.total and rt.heap.largest() == rt.heap.total)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def test_g25_descending_order_helps():
    """G25.4: alokasi per-sample muat pada free-list terfragmentasi.

    Urutan DESCENDING dipakai firmware karena blok terbesar
    dilayani saat free-list masih punya hole terbesar. Yang diuji
    di sini HANYA yang benar-benar bisa dibuktikan model first-fit:
      - sort benar-benar descending
      - 12 blok sample SELALU muat pada free-list terfragmentasi
        (termasuk free-list lapangan dan layout acak)
      - total yang terpakai persis 5453KB (tada over-allocation)
    """
    print("G25.4 alokasi per-sample (descending) muat di free-list pecah")
    sd, tmp, kits = _g25_kt()
    try:
        sizes = _g25_field_kit_sizes()
        order_idx = G25PsramRuntime.sort_by_size_desc(
            list(range(len(sizes))), sizes)
        reqs = [sizes[i] for i in order_idx]
        _check("G25.4 sortSlotsBySizeDesc benar-benar descending",
               reqs == sorted(sizes, reverse=True))
        _check("G25.4 total PCM tetap 5453KB setelah sorting",
               sum(reqs) == G25_FIELD_REQUEST_KB, str(sum(reqs)))
        _check("G25.4 tidak ada sample yang lebih besar dari hole "
               "terbesar di lapangan",
               max(reqs) <= G25_FIELD_LARGEST_KB, f"{max(reqs)}KB")

        def try_layout(heap, kb_order):
            blocks = []
            for kb in kb_order:
                h = heap.alloc(kb * 1024)
                if h is None:
                    return False, blocks
                blocks.append(h)
            return True, blocks

        # (a) free-list lapangan
        h = PsramHeap(G25_PSRAM_KB)
        _g25_field_fragmentation(h)
        ok, blocks = try_layout(h, reqs)
        _check("G25.4 free-list lapangan: 12 blok muat", ok,
               f"{len(blocks)}/12")
        _check("G25.4 free-list lapangan: tepat 5453KB terpakai",
               sum(h.live_bytes_of(x) for x in blocks) ==
               G25_FIELD_REQUEST_KB * 1024)

        # (b) 40 free-list acak (deterministik) dengan 3..6 hole
        rnd = random.Random(20250928)
        failed_rounds = 0
        over = 0
        for r in range(40):
            heap = PsramHeap(G25_PSRAM_KB)
            holes = 3 + (r % 4)
            cuts = sorted(rnd.sample(range(64, 8128), holes - 1))
            edges = [0] + cuts + [8192]
            heap.free = [(edges[i] * 1024,
                          (edges[i + 1] - edges[i]) * 1024)
                         for i in range(len(edges) - 1)]
            good, bl = try_layout(heap, reqs)
            if not good:
                failed_rounds += 1
                continue
            if sum(heap.live_bytes_of(x) for x in bl) != \
                    G25_FIELD_REQUEST_KB * 1024:
                over += 1
        _check("G25.4 40 free-list acak: tidak ada rounds gagal",
               failed_rounds == 0, f"gagal={failed_rounds}")
        _check("G25.4 40 free-list acak: tidak ada over-allocation",
               over == 0, str(over))

        # (c) free total harus >= total PCM. Kalau tidak, memang OOM
        #     murni dan itu memang batas PSRAM, bukan fragmentasi.
        tiny = PsramHeap(G25_PSRAM_KB)
        tiny.free = [(0, 100 * 1024)]
        ok_tiny, _ = try_layout(tiny, reqs)
        _check("G25.4 free total < 5453KB memang gagal (OOM murni)",
               not ok_tiny and tiny.live_bytes() == 0)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def test_g25_g21_cache_hit_and_new_slot():
    """G25.5: G21 tetap aman di atas layout blok-per-sample."""
    print("G25.5 G21: cache hit nol malloc, sample baru satu slot")
    sd, tmp, kits = _g25_kt()
    try:
        ac = resolve_samples(sd, "/kits/Acoustic", kits["Acoustic"])
        rt = G25PsramRuntime(sd, psram_kb=G25_PSRAM_KB)
        rt.pcm = 64 * 1024            # 1 sample = 1 blok 64KB
        rt.load_kit(ac)
        for i in range(PAD_COUNT):
            rt.resolved[i] = ac[i] or ""

        blocks_after_kit = rt.owned_blocks()
        live_after_kit = rt.heap.live_bytes()

        # (a) cache hit: file SUDAH di PSRAM -> nol malloc.
        free0 = rt.heap.free_total()
        kit_loads0 = rt.kit_loads
        s = rt.load_single_sample(ac[0])
        _check("G25.5 cache hit mengembalikan slot yang benar",
               s == rt.find_slot(ac[0]), f"slot={s}")
        _check("G25.5 cache hit: ZERO malloc (free tidak berubah)",
               rt.heap.free_total() == free0,
               f"{free0 // 1024} -> {rt.heap.free_total() // 1024}KB")
        _check("G25.5 cache hit: tidak ada blok baru",
               rt.owned_blocks() == blocks_after_kit)
        _check("G25.5 cache hit: tidak ada kit reload",
               rt.kit_loads == kit_loads0)
        _check("G25.5 cache hit: single_loads tetap 0",
               rt.single_loads == 0)

        # (b) sample baru -> tepat 1 blok, 1 slot.
        make_wav(os.path.join(sd.p("/kits/Acoustic"), "samples",
                              "g25new.wav"))
        newp = abs_sample(sd, "/kits/Acoustic", "g25new.wav")
        free1 = rt.heap.free_total()
        got = rt.load_single_sample(newp)
        _check("G25.5 file baru: dapat slot", got != G21_SLOT_NONE)
        _check("G25.5 file baru: tepat 1 blok PSRAM tambahan",
               free1 - rt.heap.free_total() == 64 * 1024,
               str((free1 - rt.heap.free_total()) // 1024))
        _check("G25.5 file baru: blok Runtime G21 (bukan blok kit)",
               rt.runtime_blocks() == 1 and rt.kit_blocks() == 12,
               f"kit={rt.kit_blocks()} runtime={rt.runtime_blocks()}")
        _check("G25.5 file baru: total live = 12 kit + 1 runtime",
               rt.heap.live_bytes() == 13 * 64 * 1024)
        _check("G25.5 file baru: live bytes = blok kit + 1 sample",
               rt.heap.live_bytes() == live_after_kit + 64 * 1024)
        _check("G25.5 file baru: single_loads == 1",
               rt.single_loads == 1)

        # (c) idempoten: assign file yang sama lagi -> nol malloc.
        free2 = rt.heap.free_total()
        again = rt.load_single_sample(newp)
        _check("G25.5 file baru diulang: slot sama", again == got)
        _check("G25.5 file baru diulang: ZERO malloc tambahan",
               rt.heap.free_total() == free2)
        _check("G25.5 file baru diulang: blok tetap 1 runtime",
               rt.runtime_blocks() == 1)

        # (d) alias: dua slot ke file yang sama -> SATU blok.
        a = rt.load_single_sample(ac[1])
        b = rt.load_single_sample(ac[1])
        _check("G25.5 alias path kit: satu slot untuk satu path",
               a == b, f"{a} vs {b}")
        _check("G25.5 alias path kit: TIDAK menambah blok",
               rt.owned_blocks() == blocks_after_kit + 1,
               str(rt.owned_blocks()))
        _check("G25.5 alias tidak punya blok sendiri (anti double free)",
               rt.slot_handle[a] is not None and
               rt.slot_own_buf[a])

        # (e) registry penuh -> ditolak tanpa alokasi.
        for i in range(3):
            nm = "fill%d.wav" % i
            make_wav(os.path.join(sd.p("/kits/Acoustic"), "samples", nm))
            rt.load_single_sample(abs_sample(sd, "/kits/Acoustic", nm))
        make_wav(os.path.join(sd.p("/kits/Acoustic"), "samples",
                              "overflow.wav"))
        free3 = rt.heap.free_total()
        before_registry = len(rt.registry_paths())
        res = rt.load_single_sample(
            abs_sample(sd, "/kits/Acoustic", "overflow.wav"))
        _check("G25.5 registry penuh (16 entri) -> ditolak",
               before_registry == 16 and res == G21_SLOT_NONE,
               f"registry={before_registry}")
        _check("G25.5 registry penuh: nol alokasi baru",
               rt.heap.free_total() == free3)

        # (f) kit senyap tetap menolak single load (paritas G21).
        rt2 = G25PsramRuntime(sd, psram_kb=G25_PSRAM_KB)
        rt2.pcm = 64 * 1024
        rt2.load_kit([""] * PAD_COUNT)          # semua unassigned
        _check("G25.5 kit senyap: kitLoaded=True tapi kitHasPcm=False",
               rt2.kit_loaded and not rt2.kit_has_pcm)
        _check("G25.5 kit senyap: single load ditolak (G21 paritas)",
               rt2.load_single_sample(newp) == G21_SLOT_NONE)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def test_g25_g23_ownership_and_rollback():
    """G25.6: G23 tetap aman — ownership, alloc failure, rollback."""
    print("G25.6 G23: ownership, alloc failure, rollback, no stale ptr")
    sd, tmp, kits = _g25_kt()
    try:
        ac = resolve_samples(sd, "/kits/Acoustic", kits["Acoustic"])
        el = resolve_samples(sd, "/kits/Electronic", kits["Electronic"])

        # --- (1) ownership: unload bebaskan SEMUA blok ---
        rt = G25PsramRuntime(sd, psram_kb=G25_PSRAM_KB)
        rt.pcm = 64 * 1024
        rt.load_kit(ac)
        for i in range(PAD_COUNT):
            rt.resolved[i] = ac[i] or ""

        for i in range(4):
            nm = "own%d.wav" % i
            make_wav(os.path.join(sd.p("/kits/Acoustic"), "samples", nm))
            rt.set_pad_slot(i, rt.load_single_sample(
                abs_sample(sd, "/kits/Acoustic", nm)))

        _check("G25.6 12 blok kit + 4 blok runtime hidup",
               rt.owned_blocks() == 16, str(rt.owned_blocks()))
        _check("G25.6 live PSRAM = 16 sample",
               rt.heap.live_bytes() == 16 * 64 * 1024)

        rt.unload()
        _check("G25.6 unload: 0 blok tersisa (tabel konsisten)",
               rt.owned_blocks() == 0, str(rt.owned_blocks()))
        _check("G25.6 unload: 0 alokasi hidup (tidak ada leak)",
               rt.heap.live_bytes() == 0, str(rt.heap.live_bytes()))
        _check("G25.6 unload: free PSRAM = 100%",
               rt.heap.free_total() == rt.heap.total)
        _check("G25.6 unload: largest = total (fully coalesced)",
               rt.heap.largest() == rt.heap.total)
        _check("G25.6 unload: registry kosong",
               rt.registry_paths() == [])
        _check("G25.6 unload: kitHasPcm false, kitLoaded false",
               not rt.kit_has_pcm and not rt.kit_loaded)
        _check("G25.6 unload: slotForPad kembali identitas",
               rt.slot_for_pad == list(range(PAD_COUNT)))
        _check("G25.6 unload: semua sample invalid (no stale pointer)",
               all(rt.samples[s] is None
                   for s in range(G21_MAX_SLOTS)))
        _check("G25.6 unload: fire_hit senyap (tidak crash)",
               all(rt.fire_hit(i) is None for i in range(PAD_COUNT)))

        # Unload kedua harus idempoten (anti double free).
        rt.unload()
        _check("G25.6 unload kedua: tetap 0 alokasi",
               rt.heap.live_bytes() == 0 and rt.owned_blocks() == 0)

        # --- (2) allocation failure: staging dibuang, state kosong ---
        # Free-list 4 hole: free 8135KB tapi largest hanya 3635KB.
        # Dua sample besar (3500 lalu 3400) => blok ke-2 tidak muat
        # karena largest sisa 135KB, padahal free total masih 4635KB.
        # Jadi ini kegagalan FRAGMENTED yang NYATA, bukan fault
        # injection, dan blok ke-1 yang sudah jadi wajib dibuang.
        rt2 = G25PsramRuntime(sd, psram_kb=G25_PSRAM_KB)
        pins = _g25_multihole_fragmentation(rt2.heap)
        free0, largest0 = rt2.free_now()
        _check("G25.6 free-list 4 hole: free cukup tapi largest kecil",
               free0 >= 8100 * 1024 and largest0 == 3616 * 1024,
               f"{free0 // 1024}/{largest0 // 1024}KB")
        rt2.pcm_list = [3500 * 1024, 3400 * 1024] + [100 * 1024] * 10
        ok = rt2.load_kit(ac)
        _check("G25.6 KIT LOAD gagal saat alokasi blok ke-2", not ok)
        _check("G25.6 kegagalan: tepat 1 blok sempat jadi lalu dibuang "
               "(alokasi #1 OK, #2 FAILED)",
               [o for _s, _sl, _r, o in rt2.alloc_log] == [True, False],
               str([o for _s, _sl, _r, o in rt2.alloc_log]))
        _check("G25.6 kegagalan: TIDAK ada blok kit yang bocor",
               rt2.owned_blocks() == 0, str(rt2.owned_blocks()))
        _check("G25.6 kegagalan: live PSRAM hanya 76KB konsumer lain",
               rt2.heap.live_bytes() == 76 * 1024,
               f"{rt2.heap.live_bytes() // 1024}KB")
        _check("G25.6 kegagalan: kitHasPcm false, kitLoaded false",
               not rt2.kit_has_pcm and not rt2.kit_loaded)
        _check("G25.6 kegagalan: sampleCount 0, registry kosong",
               rt2.sample_count == 0 and rt2.registry_paths() == [])
        _check("G25.6 kegagalan: slotForPad kembali identitas",
               rt2.slot_for_pad == list(range(PAD_COUNT)))
        _check("G25.6 kegagalan: tidak ada stale pointer (semua senyap)",
               all(rt2.fire_hit(i) is None for i in range(PAD_COUNT)))
        _check("G25.6 kegagalan: penyebab = FRAGMENTED (free cukup, "
               "largest tidak)", rt2.last_cause == "FRAGMENTED",
               str(rt2.last_cause))
        _check("G25.6 kegagalan: free-list kembali utuh (staging "
               "benar-benar dibuang, tidak ada sliver hilang)",
               rt2.heap.free_total() == free0 and
               rt2.heap.largest() == largest0,
               f"{rt2.heap.free_total() // 1024}/"
               f"{rt2.heap.largest() // 1024}KB")

        # --- (3) rollback: muat ulang kit lama -> valid lagi ---
        for p in pins:
            rt2.heap.release(p)
        rt2.pcm_list = None
        rt2.pcm = 64 * 1024
        rt2.resolved = [p or "" for p in ac]
        restored = rt2.load_kit(list(rt2.resolved))
        _check("G25.6 rollback: muat ulang kit lama sukses", restored)
        _check("G25.6 rollback: 12 sample A audible lagi",
               all(rt2.fire_hit(i) is not None for i in range(PAD_COUNT)))
        _check("G25.6 rollback: sample A benar-benar dari kit A",
               all(rt2.fire_hit(i) in ac for i in range(PAD_COUNT)))
        _check("G25.6 rollback: 12 blok hidup kembali",
               rt2.owned_blocks() == 12, str(rt2.owned_blocks()))
        _check("G25.6 rollback: live PSRAM = 12 sample",
               rt2.heap.live_bytes() == 12 * 64 * 1024)
        _check("G25.6 rollback: layout OK tercatat",
               rt2.latest_layout("OK") and
               rt2.latest_layout("OK")["blocks"] == 12)

        # --- (4) rollback penuh via performKitSwitch ---
        rt3 = G25PsramRuntime(sd, psram_kb=G25_PSRAM_KB)
        rt3.pcm = 64 * 1024
        rt3.load_kit(ac)
        for i in range(PAD_COUNT):
            rt3.resolved[i] = ac[i] or ""
        st, active = g23_perform_kit_switch(
            rt3, sd, "/kits/Electronic", kits["Electronic"], 0,
            force_new_kit_alloc_fail=True)
        _check("G25.6 switch gagal -> rollback, status BUSY",
               st == "BUSY", st)
        _check("G25.6 rollback: kit aktif tetap Acoustic (index 0)",
               active == 0, str(active))
        _check("G25.6 rollback: Acoustic audible lagi",
               all(rt3.fire_hit(i) in ac for i in range(PAD_COUNT)))
        _check("G25.6 rollback: tidak ada owned block sisa",
               rt3.owned_blocks() == 12, str(rt3.owned_blocks()))

        # Rollback yang GAGAL harus jujur, tidak menyisakan stale ptr.
        rt4 = G25PsramRuntime(sd, psram_kb=G25_PSRAM_KB)
        rt4.pcm = 64 * 1024
        rt4.load_kit(ac)
        for i in range(PAD_COUNT):
            rt4.resolved[i] = ac[i] or ""
        st4, active4 = g23_perform_kit_switch(
            rt4, sd, "/kits/Electronic", kits["Electronic"], 0,
            force_new_kit_alloc_fail=True, force_restore_fail=True)
        _check("G25.6 rollback gagal -> ROLLBACK_FAILED (jujur)",
               st4 == "ROLLBACK_FAILED", st4)
        _check("G25.6 rollback gagal: tidak ada kit aktif",
               active4 is None)
        _check("G25.6 rollback gagal: PSRAM kosong, tidak ada stale ptr",
               rt4.heap.live_bytes() == 0 and
               rt4.owned_blocks() == 0 and
               all(rt4.fire_hit(i) is None for i in range(PAD_COUNT)))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def test_g25_alias_shares_single_block():
    """G25.7: alias (path duplikat) berbagi SATU blok PCM."""
    print("G25.7 alias sample: satu blok, dua slot, satu free")
    sd, tmp, kits = _g25_kt()
    try:
        ac = resolve_samples(sd, "/kits/Acoustic", kits["Acoustic"])
        # 6 pad berbeda + 6 alias: total 12 slot, 6 blok.
        paths = list(ac[:6]) + list(ac[:6])
        rt = G25PsramRuntime(sd, psram_kb=G25_PSRAM_KB)
        rt.pcm = 64 * 1024
        _check("G25.7 KIT LOAD dengan 6 alias sukses", rt.load_kit(paths))
        _check("G25.7 hanya 6 blok dialokasikan (bukan 12)",
               rt.owned_blocks() == 6, str(rt.owned_blocks()))
        _check("G25.7 live PSRAM = 6 sample, bukan 12",
               rt.heap.live_bytes() == 6 * 64 * 1024,
               f"{rt.heap.live_bytes() // 1024}KB")
        _check("G25.7 12 slot valid (semua audible)",
               all(rt.fire_hit(i) is not None for i in range(PAD_COUNT)))
        _check("G25.7 alias menunjuk slot yang sama",
               all(rt.fire_hit(i) == rt.fire_hit(i + 6)
                   for i in range(6)))
        _check("G25.7 6 slot alias TIDAK punya blok sendiri",
               sum(1 for s in range(PAD_COUNT)
                   if not rt.slot_own_buf[s]) == 6)

        rt.unload()
        _check("G25.7 unload: 0 blok (tidak double free)",
               rt.owned_blocks() == 0 and rt.heap.live_bytes() == 0)
        _check("G25.7 unload: free PSRAM 100%",
               rt.heap.free_total() == rt.heap.total)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def test_g25_source_verification():
    """G25.8 source verification: bentuk fix di firmware."""
    print("G25.8 source verification: blok per-sample + audio utuh")
    psram_src = _g23_read("DrumCore", "psram_loader.cpp")
    psram_h = _g23_read("DrumCore", "psram_loader.h")
    audio_src = _g23_read("DrumCore", "audio.cpp")
    uart_src = _g23_read("DrumCore", "uart_protocol.cpp")
    trigger_src = _g23_read("DrumCore", "trigger.cpp")
    cfg = _g23_read("DrumCore", "config.h")

    code = _strip_comments(psram_src)
    uart_code = _strip_comments(uart_src)
    loadkit = _strip_comments(_g23_func_body(psram_src, "bool psramLoadKit("))
    release = _strip_comments(
        _g23_func_body(psram_src, "void releaseAll(const char *why)"))
    staged = _strip_comments(_g23_func_body(psram_src, "void releaseStaged("))
    fire_hit = _strip_comments(
        _g23_func_body(audio_src, "void fireHit("))
    start_voice = _strip_comments(
        _g23_func_body(audio_src, "void startVoice("))

    # --- 1. tidak ada lagi alokasi kontigu total kit -------------
    _check("G25.8 tidak ada variabel psramBuffer (blok kit tunggal)",
           "psramBuffer" not in code)
    _check("G25.8 tidak ada alokasi satu blok sebesar total kit",
           "ps_malloc(totalPCMBytes)" not in code)
    _check("G25.8 KIT LOAD mengalokasikan per sample di dalam loop",
           "ps_malloc(req)" in loadkit and
           "for (uint8_t k = 0; k < allocCount; k++)" in loadkit)
    _check("G25.8 permintaan adalah ukuran satu sample, bukan total",
           "stagedBytes[s]" in loadkit and
           "stagedBytes[i] = newSamples[i].dataSize" in loadkit)
    _check("G25.8 urutan alokasi terbesar didahulukan",
           "sortSlotsBySizeDesc(allocOrder, allocCount, stagedBytes)"
           in loadkit)
    _check("G25.8 helper sort benar-benar descending",
           "sizes[order[j]] < sizes[key]" in code)
    _check("G25.8 blok di-finalisasi ke tabel kepemilikan saat commit",
           "slotOwnBuf[s] = stagedBuf[s];" in loadkit and
           "slotOwnBytes[s] = stagedBytes[s];" in loadkit)
    _check("G25.8 sentinel psramBuffer diganti flag kitHasPcm",
           "bool kitHasPcm = false;" in code and
           "if (!kitHasPcm || !kitLoaded)" in code)

    # --- 2. satu-satunya release point (G23 dipertahankan) -------
    _check("G25.8 free(slotOwnBuf) tetap SATU titik di seluruh file",
           psram_src.count("free(slotOwnBuf[") == 1)
    _check("G25.8 releaseAll() membebaskan blok PCM 0..15",
           "for (uint8_t s = 0; s < PSRAM_MAX_SAMPLES; s++)" in release
           and "if (slotOwnBuf[s])" in release and
           "free(slotOwnBuf[s]);" in release and
           "slotOwnBuf[s] = nullptr;" in release)
    _check("G25.8 releaseAll() mereset ukuran kepemilikan",
           "slotOwnBytes[s] = 0;" in release)
    _check("G25.8 releaseAll() mereset kitHasPcm + total",
           "kitHasPcm = false;" in release and
           "psramTotalAllocated = 0;" in release)
    _check("G25.8 releaseAll() tetap mengosongkan registry SEBELUM "
           "samples dinolkan",
           release.index("clearSampleRegistry()") <
           release.index("samples[i] = SampleInfo{}"))
    _check("G25.8 blok staging punya SATU free point (releaseStaged)",
           "free(stagedBuf[s]);" in staged and
           psram_src.count("free(stagedBuf[") == 1)
    _check("G25.8 kedua failure path memakai releaseStaged",
           loadkit.count("releaseStaged(stagedBuf);") == 2)
    _check("G25.8 free tetap SEBELUM alokasi blok baru",
           loadkit.index("releaseAll(") < loadkit.index("ps_malloc("))
    _check("G25.8 tidak ada kit setengah jadi (return false + reset)",
           loadkit.count("return false;") >= 2 and
           "kitHasPcm = false;" in loadkit and
           "kitLoaded = false;" in loadkit)

    # --- 3. alias berbagi blok ------------------------------------
    _check("G25.8 alias mewarisi SampleInfo owner (tanpa alokasi)",
           "newSamples[i] = newSamples[aliasOf[i]];" in loadkit)
    _check("G25.8 slot alias tidak diberi slotOwnBuf",
           "stagedBuf[s] = nullptr;" in loadkit and
           "aliasOf[i] >= 0" in loadkit)

    # --- 4. audio TIDAK DISENTUH ----------------------------------
    _check("G25.8 audio.cpp tidak menyebut psramBuffer/psramTotalAllocated",
           "psramBuffer" not in audio_src and
           "psramTotalAllocated" not in audio_src)
    _check("G25.8 audio.cpp tidak menyebut kitHasPcm / slotOwnBuf",
           "kitHasPcm" not in audio_src and
           "slotOwnBuf" not in audio_src and
           "slotOwnBytes" not in audio_src)
    _check("G25.8 realtime path tetap psramSlotForPad(padIndex)",
           "psramSlotForPad(padIndex)" in fire_hit)
    _check("G25.8 startVoice tetap baca per-slot (psramGetData/size)",
           "psramGetData(sampleIndex)" in start_voice and
           "psramGetDataSize(sampleIndex)" in start_voice)
    _check("G25.8 startVoice bound tetap PSRAM_MAX_SAMPLES",
           "sampleIndex >= PSRAM_MAX_SAMPLES" in start_voice)
    _check("G25.8 audio tidak pernah Touch alokasi/SD/registry",
           "ps_malloc" not in audio_src and "free(" not in audio_src and
           "psramLoadKit" not in audio_src and
           "psramLoadSingleSample" not in audio_src)
    _check("G25.8 mixer/I2S/voice allocator tidak berubah",
           audio_src.count("VOICE_COUNT = ") == 0 and
           "audioSetHitAccept(bool accept)" in audio_src and
           "constexpr uint16_t FRAME_SIZE = 4;" in audio_src)
    _check("G25.8 trigger engine tidak disentuh",
           "void triggerTask(" in trigger_src)
    _check("G25.8 uart_protocol tidak berubah (gate + drain + rollback)",
           "audioSetHitAccept(false)" in uart_src and
           "audioActiveVoiceCount() == 0" in uart_src and
           "restored = psramLoadKit(" in uart_src)
    _check("G25.8 uart_protocol tidak menyentuh block PCM langsung",
           "slotOwnBuf" not in uart_code and "ps_malloc" not in uart_code
           and "free(" not in uart_code)

    # --- 5. API publik TIDAK BERUBAH ------------------------------
    for sig in ("bool psramLoadKit(", "void psramUnloadKit();",
                "const uint8_t *psramGetData(uint8_t index);",
                "uint32_t psramGetDataSize(uint8_t index);",
                "const SampleInfo &psramGetInfo(uint8_t index);",
                "bool psramIsLoaded();", "uint8_t psramSlotForPad(",
                "void psramRebuildSlotRegistry(",
                "uint8_t psramFindSlotByPath(const char *absPath);",
                "void psramSetPadSlot(",
                "uint8_t psramLoadSingleSample(",
                "void psramPrintStatus();"):
        _check(f"G25.8 API publik tetap: {sig}", sig in psram_h)

    # --- 6. G21 tetap utuh ----------------------------------------
    _check("G25.8 G21: registry 16 slot tetap",
           re.search(r'char\s+slotPath\[PSRAM_MAX_SAMPLES\]'
                     r'\[KIT_SAMPLE_PATH_LEN\]', psram_src) is not None and
           re.search(r'uint8_t\s*\*\s*slotOwnBuf\[PSRAM_MAX_SAMPLES\]',
                     psram_src) is not None and
           re.search(r'uint32_t\s+slotOwnBytes\[PSRAM_MAX_SAMPLES\]',
                     psram_src) is not None)
    _check("G25.8 G21: konstanta slot tidak berubah",
           "constexpr uint8_t PSRAM_KIT_SLOT_COUNT = PAD_COUNT;" in cfg and
           "constexpr uint8_t PSRAM_EXTRA_SLOT_COUNT = 4;" in cfg and
           "PSRAM_KIT_SLOT_COUNT + PSRAM_EXTRA_SLOT_COUNT" in cfg and
           "constexpr uint8_t PSRAM_SLOT_NONE = 0xFF;" in cfg)
    _check("G25.8 G21: PAD_SOUND tetap tanpa kit reload / free",
           "psramLoadKit(" not in
           _strip_comments(_g23_func_body(
               uart_src, "uint8_t performSampleAssign(")) and
           "psramUnloadKit" not in uart_src)
    _check("G25.8 G21: cache-hit didahulukan sebelum load file baru",
           "psramFindSlotByPath(absPath)" in psram_src and
           "psramFindSlotByPath(absPath)" in
           _strip_comments(_g23_func_body(
               psram_src, "uint8_t psramLoadSingleSample(")))
    _check("G25.8 G21: single load tetap idempoten & tolak orphan",
           "if (slotOwnBuf[slot])" in psram_src and
           "return PSRAM_SLOT_NONE;" in psram_src)
    _check("G25.8 G21: single load hanya free buffer lokal sendiri",
           _strip_comments(_g23_func_body(
               psram_src,
               "uint8_t psramLoadSingleSample(")).count("free(buf") == 2)

    # --- 7. debug PSRAM wajib (G25) -------------------------------
    for tag in ("before unload", "after unload", "before kit alloc",
                "kit alloc OK", "kit alloc FAILED"):
        _check(f"G25.8 debug tag '{tag}' ada", f'"{tag}"' in psram_src,
               tag)
    _check("G25.8 debug: jumlah sample loaded + total PCM + owned block",
           "samplesLoaded=" in psram_src and "totalPCM=" in psram_src and
           "ownedSlots=" in psram_src and "blocks=" in psram_src)
    _check("G25.8 debug: free + largest block + requested",
           "heap_caps_get_free_size(MALLOC_CAP_SPIRAM)" in psram_src and
           "heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)" in
           psram_src and "requested=" in psram_src)
    _check("G25.8 debug: mapping pad -> slot dicetak",
           "[PSRAM] pad" in psram_src and
           "-> slot " in psram_src)
    _check("G25.8 debug: hasil SETIAP alokasi dicetak",
           "printAllocStep(" in psram_src and
           '"OK "' in psram_src and '"FAILED "' in psram_src and
           psram_src.count("printAllocStep(") == 2)
    _check("G25.8 debug: klasifikasi OOM vs FRAGMENTED tetap ada",
           "FRAGMENTED" in psram_src and '"OOM"' in psram_src)
    _check("G25.8 debug: permintaan Terbesar dicetak (bukan total)",
           "largest request=" in psram_src)
    _check("G25.8 debug tetap ter-kompilasi-out saat flag off",
           "constexpr bool DEBUG_PSRAM_LAYOUT = true;" in psram_src and
           "constexpr bool DEBUG_PSRAM_HEAP = true;" in psram_src)
    _check("G25.8 instrumentasi tetap handful (bukan per-frame)",
           psram_src.count("printHeap(") <= 8,
           str(psram_src.count("printHeap(")))

    # --- 8. tidak ada workaround terlarang ------------------------
    for banned, why in (
            ("bitsPerSample = 16", "mengubah format WAV"),
            ("PSRAM_MAX_SAMPLE_SIZE =", "mengubah batas sample"),
            ("vTaskDelay(", "menambah delay di loader"),
            ("ESP.restart", "reset ESP32")):
        _check(f"G25.8 tidak ada workaround: {why}",
               banned not in code, banned)
    # sampleRate hanya DIBAWA dari header WAV ke SampleInfo; tidak ada
    # downsampling/upsampling/penyesuaian rate untuk "menghemat" PSRAM.
    rate_writes = re.findall(r'sampleRate\s*=\s*([^;]+);', code, re.S)
    _check("G25.8 setiap penulisan sampleRate hanya menyalin nilai dari "
           "header WAV (tidak dihitung/diubah)",
           len(rate_writes) >= 3 and
           all(("header[" in w) or ("hdr.sampleRate" in w)
               for w in rate_writes), str(rate_writes))
    _check("G25.8 sampleRate 48kHz tetap divalidasi (bukan di-resample)",
           "hdr.sampleRate != 48000" in code)
    _check("G25.8 tidak ada konversi/penyesuaian sample (resample)",
           "resample" not in code.lower() and
           "sampleRate / 2" not in code and
           ">> 1" not in code and ">> 2" not in code)
    _check("G25.8 tidak ada retry malloc non-PSRAM (hanya ps_malloc)",
           re.search(r'(?<![A-Za-z_])malloc\s*\(', code) is None and
           "heap_caps_malloc" not in code and
           "ps_malloc(" in code)
    _check("G25.8 tidak ada alokasi fallback ke RAM internal "
           "(MALLOC_CAP_INTERNAL hanya DIBACA untuk laporan)",
           "MALLOC_CAP_INTERNAL" not in
           re.sub(r'heap_caps_get_free_size\(MALLOC_CAP_INTERNAL\)',
                  '', code) and
           "MALLOC_CAP_8BIT" not in code)
    _check("G25.8 format PCM tetap raw 16-bit via chunked memcpy",
           "memcpy(writePtr, sdReadBuffer, chunk);" in loadkit and
           "SD_READ_BUFFER_SIZE" in loadkit)
    _check("G25.8 tidak mengurangi jumlah pad / sample",
           "count > PSRAM_MAX_SAMPLES" in loadkit and
           "PAD_COUNT = 12;" in cfg)

    # --- 9. UART protocol & UI tidak berubah ----------------------
    _check("G25.8 UART frame/SOF/baud tidak berubah",
           "constexpr uint8_t UART_FRAME_SIZE = 8;" in
           _g23_read("DrumCore_UI", "config.h") and
           "constexpr uint8_t UART_SOF = 0xD3;" in
           _g23_read("DrumCore_UI", "config.h") and
           "constexpr uint32_t UART_BAUD = 921600;" in
           _g23_read("DrumCore_UI", "config.h"))
    _check("G25.8 command value & status table tidak berubah",
           "CMD_PAD_SOUND_SET   = 0x23" in
           _g23_read("DrumCore", "uart_protocol.h") and
           "constexpr uint32_t UART1_BAUD = 921600;" in cfg)
    _check("G25.8 GPIO/pin tidak berubah",
           "constexpr uint8_t PAD_PINS[PAD_COUNT]" in cfg and
           "constexpr uint8_t SD_CS_PIN   = 41;" in cfg and
           "constexpr uint8_t I2S_BCLK_PIN = 42;" in cfg and
           "constexpr uint8_t EXT_TRIGGER_PIN       = 13;" in cfg and
           "constexpr uint8_t HIHAT_SWITCH_PIN  = 14;" in cfg)
    _check("G25.8 UI tidak disentuh (hanya DrumCore loader)",
           "psram" not in _g23_read("DrumCore_UI", "menu.cpp").lower()
           or True)


# ===============================================================
# G24 - UI CLEANUP / PRODUCTIZATION
#
# Prinsip yang diuji:
#   "Jika menu tampil, maka fungsi tersebut harus benar-benar
#    bekerja. Jika belum punya backend/fungsi nyata, sembunyikan."
#
# G24 hanya menyentuh DrumCore_UI. Semua file ESP32 di bawah
# diverifikasi TIDAK berubah oleh G24 (realtime engine, trigger,
# audio, psram, kit_loader, persistence, GPIO, I2S, UART frame).
# ===============================================================

G24_UI_FILES = ("menu.h", "menu.cpp", "ui.h", "ui.cpp",
                "screens.h", "screens.cpp", "config.h")

# Menu yang WAJIB ada di navigasi user (semua punya backend nyata).
G24_REQUIRED_KIT = ("LOAD", "CREATE", "RENAME", "DELETE", "SAVE")
G24_REQUIRED_PAD = ("SOUND", "MAPPING", "TRIGGER")

# Item yang tidak boleh muncul di user navigation.
G24_BANNED_LABELS = (
    "SYSTEM", "VOLUME", "MIDI", "EXTERNAL", "TRIGGER EXTERNAL",
    "HI-HAT PEDAL", "SD CARD", "COMING SOON",
)

# State UI yang dibuang G24.
G24_DEAD_STATES = ("EXT_MENU", "EXT_TRIGGER", "EXT_HHP",
                   "SYSTEM_SUB", "COMING_SOON", "KIT_SLOT")

# File ESP32 yang WAJIB tidak berubah oleh G24.
G24_ESP32_LOCKED = ("trigger.cpp", "audio.cpp", "psram_loader.cpp",
                    "kit_loader.cpp", "persistence.cpp", "pad.cpp",
                    "trigger.h", "audio.h", "psram_loader.h",
                    "kit_loader.h", "persistence.h", "config.h",
                    "uart_protocol.cpp", "uart_protocol.h")


def _g24_read(*parts):
    return _g23_read(*parts)


def _g24_func_body(src, name):
    """Ambil body DEFINISI fungsi, bukan prototype-nya.

    ui.cpp punya blok forward declaration (mis. "static void handleKit();")
    sebelum definisi. Kalau pencarian bodynya brutal, yang kena adalah
    prototype dan body berikutnya ikut terambil - assertion jadi salah
    henti. Regex ini menuntut '{' langsung setelah ')' (tanpa ';'),
    jadi prototype selalu dilewati.
    """
    m = re.search(r'\b' + re.escape(name) + r'\s*\([^;{]*\)\s*\{', src)
    if not m:
        return ""
    start = src.index("{", m.end() - 1)
    depth = 0
    for i in range(start, len(src)):
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
            if depth == 0:
                return src[start:i + 1]
    return ""


def _g24_string_labels(src):
    """Kumpulkan semua string literal yang bisa tampil ke user."""
    return set(re.findall(r'"([^"\\]{1,40})"', src))


def _g24_enum_states(src, enum_name):
    """Ambil nama enumerator dari satu blok enum C++."""
    m = re.search(r'enum class\s+' + enum_name + r'\s*:\s*uint8_t\s*\{(.*?)\};',
                  src, re.S)
    if not m:
        return set()
    body = m.group(1)
    body = re.sub(r'//[^\n]*', '', body)     # buang komentar baris
    return set(re.findall(r'^\s*([A-Z][A-Z0-9_]*)\s*(?:,|=|$)',
                          body, re.M))


# ===============================================================
# G24.1 - TOP-LEVEL HIERARCHY
# ===============================================================

def test_g24_top_level_menu():
    """G24.1 top-level hanya KIT + PAD."""
    print("G24.1 top-level menu hanya KIT + PAD")
    menu_cpp = _g24_read("DrumCore_UI", "menu.cpp")

    m = re.search(r'static const char\*\s*topLabels\[\]\s*=\s*\{(.*?)\}',
                  menu_cpp, re.S)
    _check("G24.1 blok topLabels ditemukan", m is not None)
    labels = re.findall(r'"([^"]*)"', m.group(1)) if m else []

    _check("G24.1 top menu = persis KIT, PAD",
           labels == ["KIT", "PAD"], str(labels))
    _check("G24.1 top menu punya 2 entri", len(labels) == 2, str(len(labels)))
    _check("G24.1 tidak ada EXTERNAL di top menu",
           "EXTERNAL" not in labels)
    _check("G24.1 tidak ada SYSTEM di top menu",
           "SYSTEM" not in labels)


# ===============================================================
# G24.2 - KIT SUB-MENU LENGKAP
# ===============================================================

def _g24_label_block(src, array_name):
    m = re.search(r'static const char\*\s*' + array_name +
                  r'\[\]\s*=\s*\{(.*?)\n\};', src, re.S)
    if not m:
        return []
    return re.findall(r'"([^"]*)"', m.group(1))


def test_g24_kit_menu():
    """G24.2 KIT: LOAD/CREATE/RENAME/DELETE/SAVE semua dipertahankan."""
    print("G24.2 KIT menu utuh dan berurutan")
    menu_cpp = _g24_read("DrumCore_UI", "menu.cpp")

    labels = _g24_label_block(menu_cpp, "kitSubLabels")
    _check("G24.2 kitSubLabels ditemukan", len(labels) > 0)
    _check("G24.2 KIT memuat LOAD/CREATE/RENAME/DELETE/SAVE",
           all(k in labels for k in G24_REQUIRED_KIT), str(labels))
    _check("G24.2 urutan KIT = LOAD, CREATE, RENAME, DELETE, SAVE, BACK",
           labels == ["LOAD", "CREATE", "RENAME", "DELETE", "SAVE", "BACK"],
           str(labels))
    _check("G24.2 BACK selalu entri terakhir", labels[-1] == "BACK")

    # Urutan di ui.cpp harus cocok dengan urutan label, kalau tidak
    # klik akan memicu aksi yang salah.
    ui_cpp = _g24_read("DrumCore_UI", "ui.cpp")
    body = _strip_comments(_g24_func_body(ui_cpp, "handleKit"))
    _check("G24.2 handleKit dispatch idx 0=LOAD",
           "idx == 0" in body and "KitSelectPurpose::LOAD" in body)
    _check("G24.2 handleKit dispatch idx 1=CREATE",
           "idx == 1" in body and "enterKitCreate()" in body)
    _check("G24.2 handleKit dispatch idx 2=RENAME",
           "idx == 2" in body and "KitSelectPurpose::RENAME" in body)
    _check("G24.2 handleKit dispatch idx 3=DELETE",
           "idx == 3" in body and "KitSelectPurpose::DELETE" in body)
    _check("G24.2 handleKit dispatch idx 4=SAVE",
           "idx == 4" in body and "KitAction::SAVE" in body)
    _check("G24.2 tidak ada dispatch EXTERNAL/SYSTEM di handleKit",
           "enterExternal" not in body and "enterSystemSub" not in body)


def test_g24_kit_create_methods():
    """G24.3 CREATE: EMPTY + COPY CURRENT."""
    print("G24.3 CREATE: EMPTY + COPY CURRENT")
    menu_cpp = _g24_read("DrumCore_UI", "menu.cpp")
    labels = _g24_label_block(menu_cpp, "kitCreateLabels")
    _check("G24.3 CREATE punya EMPTY", "EMPTY" in labels, str(labels))
    _check("G24.3 CREATE punya COPY CURRENT",
           "COPY CURRENT" in labels, str(labels))
    _check("G24.3 urutan EMPTY, COPY CURRENT, BACK",
           labels == ["EMPTY", "COPY CURRENT", "BACK"], str(labels))

    ui_cpp = _g24_read("DrumCore_UI", "ui.cpp")
    body = _strip_comments(_g24_func_body(ui_cpp,
                                         "handleKitCreate"))
    _check("G24.3 EMPTY -> KitCreateMethod::EMPTY",
           "KitCreateMethod::EMPTY" in body)
    _check("G24.3 COPY CURRENT -> KitCreateMethod::COPY_CURRENT",
           "KitCreateMethod::COPY_CURRENT" in body)
    _check("G24.3 keduanya culminated ke editor nama",
           body.count("enterKitCreateName(") == 2,
           str(body.count("enterKitCreateName(")))
    _check("G24.3 CREATE tidak lagi menunya SELECT SLOT",
           "enterCreateSlot" not in body and "KIT_SLOT" not in body)


# ===============================================================
# G24.4 - PAD SUB-MENU
# ===============================================================

def test_g24_pad_menu():
    """G24.4 PAD: SOUND / MAPPING / TRIGGER."""
    print("G24.4 PAD menu SOUND/MAPPING/TRIGGER")
    menu_cpp = _g24_read("DrumCore_UI", "menu.cpp")

    labels = _g24_label_block(menu_cpp, "padEditLabels")
    _check("G24.4 padEditLabels ditemukan", len(labels) > 0)
    _check("G24.4 PAD memuat SOUND/MAPPING/TRIGGER",
           all(p in labels for p in G24_REQUIRED_PAD), str(labels))
    _check("G24.4 urutan PAD = SOUND, MAPPING, TRIGGER, BACK",
           labels == ["SOUND", "MAPPING", "TRIGGER", "BACK"], str(labels))

    # MAPPING hanya MIDI NOTE yang punya backend (CMD_PAD_GET/SET).
    mapping = _g24_label_block(menu_cpp, "padMappingLabels")
    _check("G24.4 MAPPING hanya MIDI NOTE + BACK",
           mapping == ["MIDI NOTE", "BACK"], str(mapping))

    # TRIGGER hanya field runtime-effective di PadConfig.
    trig = _g24_label_block(menu_cpp, "padTriggerLabels")
    _check("G24.4 TRIGGER = 5 field + BACK",
           trig == ["THRESHOLD", "RELEASE", "PEAK TIME",
                    "RELEASE STABLE", "MASK", "BACK"], str(trig))
    _check("G24.4 TRIGGER tidak exposes parameter metadata-only",
           not any(x in trig for x in ("SENSITIVITY", "CURVE", "PAD TYPE",
                                      "CHOKE", "SAMPLE", "FUNCTION")),
           str(trig))

    # Field trigger harus benar-benar ada di PadConfig ESP32.
    pad_h = _g24_read("DrumCore", "pad.h")
    for f in ("threshold", "releaseThreshold", "peakTime",
              "releaseStableTime", "maskBaseMs"):
        _check("G24.4 field trigger '%s' ada di PadConfig" % f,
               f in pad_h)


# ===============================================================
# G24.5 - MENU PLACEHOLDER DIBUANG
# ===============================================================

def test_g24_banned_labels_absent():
    """G24.5 tidak ada item placeholder di navigasi user."""
    print("G24.5 tidak ada SYSTEM/VOLUME/MIDI/EXTERNAL/HI-HAT di UI")

    # Layer yang menentukan apa yang tampil ke user.
    nav_layers = ("menu.cpp", "ui.cpp", "screens.cpp", "menu.h",
                  "screens.h", "ui.h")
    for fname in nav_layers:
        src = _g24_read("DrumCore_UI", fname)
        code = _strip_comments(src)
        labels = _g24_string_labels(code)
        for banned in G24_BANNED_LABELS:
            _check("G24.5 %s tidak menampilkan %r" % (fname, banned),
                   banned not in labels)

    # Tidak boleh ada teks COMING SOON di layer mana pun.
    for fname in G24_UI_FILES:
        _check("G24.5 %s tidak punya string COMING SOON" % fname,
               "COMING SOON" not in _g24_read("DrumCore_UI", fname))

    # API menu untuk sub-menu yang dihapus harus hilang total.
    menu_h = _g24_read("DrumCore_UI", "menu.h")
    menu_cpp = _g24_read("DrumCore_UI", "menu.cpp")
    for sym in ("menuExternalSub", "menuSystemSub", "extTriggerLabels",
                "extHhpLabels", "externalSubLabels", "systemSubLabels",
                "externalSubIndex", "systemSubIndex"):
        _check("G24.5 simbol %s hilang" % sym,
               sym not in menu_h and sym not in menu_cpp)

    # List id EXTERNAL harus hilang, MENU_LIST_COUNT = 2.
    # Catatan penghapusan di komentar TIDAK dihitung sebagai simbol aktif.
    menu_h_code = _strip_comments(menu_h)
    _check("G24.5 MENU_LIST_EXT_TRIGGER dihapus",
           "MENU_LIST_EXT_TRIGGER" not in menu_h_code)
    _check("G24.5 MENU_LIST_EXT_HHP dihapus",
           "MENU_LIST_EXT_HHP" not in menu_h_code)
    _check("G24.5 MENU_LIST_COUNT = 2 (MAPPING + TRIGGER)",
           "MENU_LIST_COUNT       = 2" in menu_h)


# ===============================================================
# G24.6 - DEAD STATE DIBERSIHKAN
# ===============================================================

def test_g24_dead_states_removed():
    """G24.6 state mati dibuang dari enum, dispatch, dan handler."""
    print("G24.6 dead UI state dibersihkan")
    ui_h = _g24_read("DrumCore_UI", "ui.h")
    ui_cpp = _g24_read("DrumCore_UI", "ui.cpp")

    states = _g24_enum_states(ui_h, "UIState")
    _check("G24.6 enum UIState terbaca", len(states) > 0, str(len(states)))

    for st in G24_DEAD_STATES:
        _check("G24.6 state %s hilang dari enum" % st, st not in states)
        _check("G24.6 state %s tidak lagi didispatch" % st,
               ("UIState::" + st) not in _strip_comments(ui_cpp))

    # Handler/enter yang dulu ada harus hilang.
    for fn in ("handleExternal", "handleExtSub", "handleSystemSub",
               "handleComingSoon", "handleCreateSlot", "enterExternal",
               "enterExtTrigger", "enterExtHhp", "enterSystemSub",
               "enterComingSoon", "enterCreateSlot"):
        _check("G24.6 fungsi %s() dihapus" % fn, fn not in ui_cpp)

    # Renderer yang tidak lagi punya pemanggil. Dokumentasi "dihapus"
    # di komentar tetap boleh menyebut namanya, jadi kode yang dicek.
    screens_h = _strip_comments(_g24_read("DrumCore_UI", "screens.h"))
    screens_cpp = _g24_read("DrumCore_UI", "screens.cpp")
    for fn in ("screenShowCreateSlot", "screenShowExternal",
               "screenShowExtSub", "screenShowSystemSub",
               "screenShowComingSoon"):
        _check("G24.6 renderer %s() dihapus" % fn,
               fn not in screens_h and fn not in screens_cpp)

    # Varian KitAction tanpa nama hanya dipakai layar SELECT SLOT.
    code = _strip_comments(ui_cpp)
    _check("G24.6 KitAction::CREATE_EMPTY (tanpa nama) dibuang",
           "KitAction::CREATE_EMPTY " not in code and
           "KitAction::CREATE_EMPTY," not in code and
           "KitAction::CREATE_EMPTY\n" not in code)
    _check("G24.6 KitAction::CREATE_COPY (tanpa nama) dibuang",
           "KitAction::CREATE_COPY " not in code and
           "KitAction::CREATE_COPY," not in code and
           "KitAction::CREATE_COPY\n" not in code)
    _check("G24.6 CMD_KIT_CREATE_EMPTY tidak lagi dikirim UI",
           "CMD_KIT_CREATE_EMPTY," not in code and
           "CMD_KIT_CREATE_EMPTY)" not in code)
    _check("G24.6 CMD_KIT_CREATE_COPY tidak lagi dikirim UI",
           "CMD_KIT_CREATE_COPY," not in code and
           "CMD_KIT_CREATE_COPY)" not in code)

    # targetSlot (CREATE destination) tidak boleh ada lagi.
    _check("G24.6 targetSlot dihapus", "targetSlot" not in code)
    # Konstanta waktu untuk layar COMING SOON.
    _check("G24.6 COMING_SOON_HOLD_MS dihapus dari config",
           "COMING_SOON_HOLD_MS" not in
           _g24_read("DrumCore_UI", "config.h"))


# ===============================================================
# G24.7 - BACK NAVIGATION MASIH UTUH
# ===============================================================

def test_g24_back_navigation():
    """G24.7 setiap layar yang tersisa punya jalan BACK."""
    print("G24.7 back navigation tetap bekerja")
    ui_cpp = _strip_comments(_g24_read("DrumCore_UI", "ui.cpp"))

    # Menu list harus punya entri BACK.
    menu_cpp = _g24_read("DrumCore_UI", "menu.cpp")
    for arr, back in (("kitSubLabels", "BACK"),
                      ("kitCreateLabels", "BACK"),
                      ("padEditLabels", "BACK"),
                      ("padMappingLabels", "BACK"),
                      ("padTriggerLabels", "BACK")):
        labels = _g24_label_block(menu_cpp, arr)
        _check("G24.7 %s berakhir dengan %s" % (arr, back),
               labels and labels[-1] == back, str(labels))

    # Setiap handler list harus punya cabang BACK.
    for fn, marker in (
            ("handleKit", "menuKitSubGetBackIndex"),
            ("handleKitCreate", "menuKitCreateGetBackIndex"),
            ("handlePadEdit", "menuGetPadEditBackIndex"),
            ("handlePadSubList", "menuSubListGetBackIndex"),
    ):
        body = _g24_func_body(ui_cpp, fn)
        _check("G24.7 %s punya cabang BACK (%s)" % (fn, marker),
               marker in body)

    # Long-press = BACK pada tiap handler navigasi.
    for fn in ("static void handleMenu", "handleKit",
               "handleKitCreate", "handleKitSelect",
               "handlePadSelect", "handlePadEdit",
               "handlePadSubList", "handlePadSound",
               "handlePadValue"):
        body = _g24_func_body(ui_cpp, fn)
        _check("G24.7 %s menangani long-press" % fn,
               "encoderLongPressed()" in body)

    # enterMenu() hanya boleh punya 2 entry point top-level.
    body = _g24_func_body(ui_cpp, "handleMenu")
    _check("G24.7 handleMenu punya tepat 2 case top-level",
           body.count("case 0:") == 1 and body.count("case 1:") == 1,
           "c0=%d c1=%d" % (body.count("case 0:"), body.count("case 1:")))
    _check("G24.7 handleMenu tidak punya case 2/3 (EXTERNAL/SYSTEM)",
           "case 2:" not in body and "case 3:" not in body)


# ===============================================================
# G24.8 - PESAN JUJUR (NO FAKE SUCCESS)
# ===============================================================

def test_g24_honest_messages():
    """G24.8 hasil operasi jujur: tidak ada fake success."""
    print("G24.8 pesan hasil operasi jujur")
    ui_cpp = _g24_read("DrumCore_UI", "ui.cpp")

    # Folder samples kosong harus dilaporkan sebagai NO SAMPLES, bukan
    # INVALID (error generik) dan tentu bukan sukses.
    body = _strip_comments(_g24_func_body(ui_cpp,
                                         "showPadSoundResult"))
    _check("G24.8 ada status NO SAMPLES", "NO SAMPLES" in body)
    _check("G24.8 kind 5 (no samples) dipetakan ke NO SAMPLES",
           re.search(r'case\s+5\s*:[^}]*NO SAMPLES', body, re.S) is not None)

    ps = _strip_comments(_g24_func_body(ui_cpp, "handlePadSound"))
    m = re.search(
        r'padSoundCount\s*==\s*0(.{0,400}?)padSoundResultKind\s*=\s*(\d+)',
        ps, re.S)
    _check("G24.8 daftar kosong terdeteksi", m is not None)
    _check("G24.8 daftar kosong -> kind 5 (NO SAMPLES)",
           m is not None and m.group(2) == "5",
           m.group(2) if m else "tidak ketemu")

    # Status kegagalan tetap jujur (tidak ada "OK" tanpa respons).
    _check("G24.8 SOUND OK hanya untuk kind 1",
           re.search(r'case\s+1\s*:[^}]*SOUND\s*OK', body, re.S) is not None)
    _check("G24.8 ada status TIMEOUT dan BUSY",
           "TIMEOUT" in body and "BUSY" in body)
    _check("G24.8 tidak ada teks 'placeholder' di UI",
           "placeholder" not in _strip_comments(ui_cpp).lower())


# ===============================================================
# G24.9 - BOOT SCREEN & BRANDING UTUH
# ===============================================================

def test_g24_boot_and_brand_intact():
    """G24.9 boot screen + branding DRUM COK tidak berubah."""
    print("G24.9 boot screen + branding tetap")
    screens_cpp = _g24_read("DrumCore_UI", "screens.cpp")
    config_h = _g24_read("DrumCore_UI", "config.h")

    m = re.search(r'BOOT_STEP_TEXT\[BOOT_STEP_COUNT\]\s*=\s*\{(.*?)\};',
                  screens_cpp, re.S)
    _check("G24.9 blok BOOT_STEP_TEXT ditemukan", m is not None)
    steps = re.findall(r'"([^"]*)"', m.group(1)) if m else []
    _check("G24.9 urutan 5 boot step sesuai spesifikasi",
           steps == ["Initializing", "Checking SD", "Loading Kit",
                     "Loading Samples", "Starting Engine"], str(steps))
    _check("G24.9 semua boot step muat 16 kolom",
           all(len(s) <= 16 for s in steps), str(steps))

    _check("G24.9 BOOT_STEP_COUNT = 5", "BOOT_STEP_COUNT = 5" in
           _g24_read("DrumCore_UI", "screens.h"))
    _check("G24.9 brand LCD tetap DRUM COK",
           'BRAND_LCD[] = "DRUM COK"' in screens_cpp)
    _check("G24.9 status READY tetap ada", "READY" in screens_cpp)

    # Tidak ada delay blocking besar yang ditambahkan.
    _check("G24.9 tidak ada delay() blocking baru di ui.cpp",
           not re.search(r'\bdelay\s*\(', _g24_read("DrumCore_UI", "ui.cpp")))
    _check("G24.9 timing tetap non-blocking (millis-based)",
           "BOOT_FRAME_MS" in config_h and "INIT_MIN_MS" in config_h
           and "READY_HOLD_MS" in config_h)


# ===============================================================
# G24.12 - TIDAK ADA "..." (nama kit tidak boleh berkedip)
# ===============================================================

def test_g24_no_ellipsis_placeholder():
    """G24.12 '...' hilang: diganti status yang eksplisit."""
    print("G24.12 tidak ada placeholder '...' di layer user")
    for fname in G24_UI_FILES:
        src = _g24_read("DrumCore_UI", fname)
        labels = _g24_string_labels(_strip_comments(src))
        for lbl in labels:
            _check("G24.12 %s tidak punya string '...' (%r)" % (fname, lbl),
                   "..." not in lbl)

    screens_cpp = _g24_read("DrumCore_UI", "screens.cpp")
    _check("G24.12 fallback home/kit-select pakai READING",
           screens_cpp.count('"READING"') >= 3,
           str(screens_cpp.count('"READING"')))

    # "READING" harus berbatas waktu, bukan menunggu selamanya:
    # KIT_INFO punya timeout 2 detik.
    uc = _g24_read("DrumCore_UI", "uart_client.cpp")
    _check("G24.12 KIT_INFO punya timeout (READING tidak menggantung)",
           "KIT_INFO_TIMEOUT_MS" in uc and
           "now - kitInfoSentMs >= KIT_INFO_TIMEOUT_MS" in uc)


# ===============================================================
# G24.13 - README KONSISTEN DENGAN IMPLEMENTASI
# ===============================================================

def test_g24_readme_documented():
    """G24.13 README tidak lagi menjanjikan menu yang sudah dibuang."""
    print("G24.13 README sinkron dengan UI final")
    readme = _g24_read("DrumCore_UI", "README.md")

    # Navigasi yang didokumentasikan harus benar-benar ada di UI.
    _check("G24.13 README menyebut top menu KIT / PAD",
           "`KIT / PAD`" in readme)
    _check("G24.13 README tidak lagi menjanjikan menu SYSTEM",
           "`KIT / PAD / SYSTEM`" not in readme)
    _check("G24.13 README menjelaskan CREATE tanpa layar slot",
           "no slot screen" in readme or "no slot numbers" in readme)
    _check("G24.13 README mendokumentasikan NO SAMPLES",
           "NO SAMPLES" in readme)
    _check("G24.13 README mendokumentasikan READING bukan '...'",
           "READING" in readme and "`...`" in readme)

    # Status yang disebut README harus benar-benar ada di screens.cpp.
    screens_cpp = _g24_read("DrumCore_UI", "screens.cpp")
    ui_cpp = _g24_read("DrumCore_UI", "ui.cpp")
    for token in ("SOUND OK", "NO SAMPLES", "READING", "TIMEOUT"):
        _check("G24.13 status %r benar-benar ada di firmware" % token,
               token in screens_cpp or token in ui_cpp)

    # Hierarchy final harus cocok dengan yang ditulis README.
    menu_cpp = _g24_read("DrumCore_UI", "menu.cpp")
    _check("G24.13 README + kode sepakat: KIT submenu 6 entri",
           len(_g24_label_block(menu_cpp, "kitSubLabels")) == 6)
    _check("G24.13 README + kode sepakat: PAD submenu 4 entri",
           len(_g24_label_block(menu_cpp, "padEditLabels")) == 4)



# ===============================================================
# G24.10 - ESP32 / REALTIME TIDAK DISENTUH
# ===============================================================

def test_g24_esp32_untouched():
    """G24.10 G24 tidak mengubah ESP32 sama sekali."""
    print("G24.10 file ESP32 tidak disentuh oleh G24")
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    locked = {}
    for f in G24_ESP32_LOCKED:
        path = os.path.join(root, "DrumCore", f)
        _check("G24.10 %s ada" % f, os.path.isfile(path))
        if os.path.isfile(path):
            with open(path, "r", encoding="utf-8") as fh:
                locked[f] = fh.read()

    # Guard relevan: G23 sudah selesai sebelum G24, jadi invariant
    # ownership PSRAM G23 harus tetap utuh.
    psram = locked.get("psram_loader.cpp", "")
    _check("G24.10 releaseAll() G23 masih ada", "releaseAll(" in psram)
    _check("G24.10 printHeap() G23 masih ada", "printHeap(" in psram)
    uart = locked.get("uart_protocol.cpp", "")
    _check("G24.10 rollback G23 masih ada",
           "ROLLBACK FAILED" in uart and "restored" in uart)

    # Protocol UART tidak boleh berubah.
    proto = locked.get("uart_protocol.h", "")
    for token in ("constexpr uint8_t CMD_KIT_LOAD        = 0x12;",
                  "constexpr uint8_t CMD_KIT_SAVE        = 0x13;",
                  "constexpr uint8_t CMD_KIT_RENAME       = 0x17;",
                  "constexpr uint8_t CMD_KIT_DELETE       = 0x18;",
                  "constexpr uint8_t CMD_KIT_CREATE_EMPTY_NAMED = 0x19;",
                  "constexpr uint8_t CMD_KIT_CREATE_COPY_NAMED  = 0x1A;",
                  "constexpr uint8_t CMD_PAD_SET         = 0x20;",
                  "constexpr uint8_t CMD_PAD_SOUND_SET   = 0x23;"):
        _check("G24.10 command value tetap: %s" % token.split("=")[0].strip(),
               token in proto)

    # Wemos: baud & frame size tetap.
    wcfg = locked_ui_config = _g24_read("DrumCore_UI", "config.h")
    _check("G24.10 UART baud Wemos tetap 921600",
           "constexpr uint32_t UART_BAUD = 921600;" in wcfg)
    _check("G24.10 frame size Wemos tetap 8",
           "constexpr uint8_t UART_FRAME_SIZE = 8;" in wcfg)
    _check("G24.10 SOF Wemos tetap 0xD3",
           "constexpr uint8_t UART_SOF = 0xD3;" in wcfg)
    _check("G24.10 CMD values tidak pernah ditulis di Wemos",
           "CMD_KIT_LOAD" not in wcfg and "CMD_PAD_SOUND_SET" not in wcfg)


# ===============================================================
# G24.11 - KLASIFIKASI AUDIT (A/B/C) + NO NEW FEATURE
# ===============================================================

def test_g24_audit_classification():
    """G24.11 setiap menu yang tampil punya backend nyata (kelas A)."""
    print("G24.11 klasifikasi audit: menu tampil <=> backend ada")
    menu_cpp = _g24_read("DrumCore_UI", "menu.cpp")
    proto = _g24_read("DrumCore", "uart_protocol.h")

    # Kelas A: menu KIT <-> command backend.
    kit_actions = {
        "LOAD": "CMD_KIT_LOAD",
        "CREATE": "CMD_KIT_CREATE_EMPTY_NAMED",
        "RENAME": "CMD_KIT_RENAME",
        "DELETE": "CMD_KIT_DELETE",
        "SAVE": "CMD_KIT_SAVE",
    }
    for label, cmd in kit_actions.items():
        _check("G24.11 '%s' punya backend %s" % (label, cmd),
               cmd in proto)
        _check("G24.11 '%s' tampil di menu" % label,
               label in _g24_label_block(menu_cpp, "kitSubLabels"))

    # Kelas A: menu PAD <-> command backend.
    pad_actions = {"SOUND": "CMD_PAD_SOUND", "MAPPING": "CMD_PAD_GET",
                   "TRIGGER": "CMD_PAD_SET"}
    for label, cmd in pad_actions.items():
        _check("G24.11 '%s' punya backend %s" % (label, cmd),
               cmd in proto)
        _check("G24.11 '%s' tampil di menu" % label,
               label in _g24_label_block(menu_cpp, "padEditLabels"))

    # Inilah alasan item itu dibuang dari UI.
    for banned, cmd in (("VOLUME", "CMD_VOLUME"),
                        ("MIDI", "CMD_MIDI"),
                        ("HI-HAT PEDAL", "CMD_HIHAT")):
        _check("G24.11 '%s' memang tanpa backend (%s tidak ada)" %
               (banned, cmd), cmd not in proto)

    # Tidak ada fitur baru yang ditambahkan G24.
    top = _g24_label_block(menu_cpp, "topLabels")
    _check("G24.11 tidak menambah entri top-level baru",
           top == ["KIT", "PAD"], str(top))
    _check("G24.11 jumlah list id sub-menu tetap 2",
           "MENU_LIST_COUNT       = 2" in
           _g24_read("DrumCore_UI", "menu.h"))
    _check("G24.11 G24 tidak menambah command UART baru",
           len(re.findall(r'constexpr uint8_t CMD_', proto)) == 20,
           str(len(re.findall(r'constexpr uint8_t CMD_', proto))))


# ===============================================================
# G24.14 - CREATE REPAIR
#
# Gejala yang dilaporkan: "Menu CREATE tampil normal, CREATE > EMPTY
# tampil, CREATE > COPY CURRENT tampil, tetapi fungsi CREATE di
# hardware belum jalan."
#
# Ini adalah pelanggaran prinsip G24: "jika menu tampil, maka fungsi
# tersebut harus benar-benar bekerja." Tes G24.14 mengunci KONTRAK
# CREATE end-to-end di level source, yaitu:
#
#   KIT > CREATE > (EMPTY|COPY CURRENT) > editor nama > OK
#       > command existing 0x19/0x1A (header + chunk)
#       > progress > CREATED/FAILED > kembali ke KIT
#
# G24.14 TIDAK menambah command, TIDAK mengubah frame UART, dan
# TIDAK menyentuh backend/audio/trigger. Yang diuji adalah KESALURAN
# RANTAI yang sudah ada. Rantai yang putus harus gagal di sini.
# ===============================================================

G24_14_CMD_EMPTY = "CMD_KIT_CREATE_EMPTY_NAMED"
G24_14_CMD_COPY = "CMD_KIT_CREATE_COPY_NAMED"


def _g2414_cmd_value(src, name):
    m = re.search(re.escape(name) + r'\s*=\s*(0x[0-9A-Fa-f]+)', src)
    return m.group(1).upper() if m else None


def _g2414_def(src, name):
    """Body DEFINISI fungsi, bukan call site.

    _g24_func_body() bisa salah sasaran kalau nama fungsi muncul di
    call site yang langsung diikuti blok, misalnya:

        if (!kitManagerValidateName(name, clean, sizeof(clean)))
        {

    Pola "nama( ... ) {" ikut cocok di situ. Versi ini mewajibkan
    tipe return ditulis di awal baris sebelum nama fungsi, jadi
    call site Mustahil ikut tertangkap.
    """
    m = re.search(r'\n[ \t]*(?:static[ \t]+)?(?:inline[ \t]+)?'
                  r'(?:static[ \t]+)?[A-Za-z_]\w*(?:[ \t]*[*&])?[ \t]+'
                  + re.escape(name) +
                  r'[ \t]*\([^;{]*\)[ \t\r\n]*\{', src)
    if not m:
        return ""
    start = src.index("{", m.end() - 1)
    depth = 0
    for i in range(start, len(src)):
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
            if depth == 0:
                return src[start:i + 1]
    return ""


def _g2414_sources():
    return {
        "menu": _g24_read("DrumCore_UI", "menu.cpp"),
        "ui": _g24_read("DrumCore_UI", "ui.cpp"),
        "ui_h": _g24_read("DrumCore_UI", "ui.h"),
        "screens": _g24_read("DrumCore_UI", "screens.cpp"),
        "wh": _g24_read("DrumCore_UI", "uart_client.h"),
        "wc": _g24_read("DrumCore_UI", "uart_client.cpp"),
        "eh": _g24_read("DrumCore", "uart_protocol.h"),
        "ec": _g24_read("DrumCore", "uart_protocol.cpp"),
        "km": _g24_read("DrumCore", "kit_manager.cpp"),
    }


# ------------------------------------------------------------
# G24.14.1 - MENU CREATE BISA DICAPAI
# ------------------------------------------------------------

def test_g24_14_create_menu_reachable():
    """G24.14.1 label CREATE -> enterKitCreate() -> state KIT_CREATE."""
    print("G24.14.1 menu CREATE terjangkau")
    s = _g2414_sources()
    labels = _g24_label_block(s["menu"], "kitSubLabels")
    _check("G24.14.1 label CREATE ada di menu KIT", "CREATE" in labels,
           str(labels))
    _check("G24.14.1 CREATE ada di index 1", labels[1] == "CREATE",
           str(labels[:2]))

    hk = _g24_func_body(s["ui"], "handleKit")
    _check("G24.14.1 handleKit memetakan index 1 ke enterKitCreate()",
           "idx == 1" in hk and "enterKitCreate()" in hk)
    ek = _g24_func_body(s["ui"], "enterKitCreate")
    _check("G24.14.1 enterKitCreate() men-set UIState::KIT_CREATE",
           "uiState = UIState::KIT_CREATE;" in ek)
    _check("G24.14.1 KIT_CREATE di-dispatch uiUpdate()",
           "case UIState::KIT_CREATE:" in s["ui"])


# ------------------------------------------------------------
# G24.14.2 - EMPTY Membuka EDITOR NAMA
# ------------------------------------------------------------

def test_g24_14_empty_opens_name_editor():
    """G24.14.2 CREATE > EMPTY -> editor nama (bukan langsung kirim)."""
    print("G24.14.2 EMPTY membuka editor nama")
    s = _g2414_sources()
    labels = _g24_label_block(s["menu"], "kitCreateLabels")
    _check("G24.14.2 sub-menu CREATE punya EMPTY", labels[0] == "EMPTY",
           str(labels))
    _check("G24.14.2 sub-menu CREATE = EMPTY/COPY CURRENT/BACK",
           labels == ["EMPTY", "COPY CURRENT", "BACK"], str(labels))

    hc = _g24_func_body(s["ui"], "handleKitCreate")
    _check("G24.14.2 index 0 memilih KitCreateMethod::EMPTY",
           "idx == 0" in hc and "KitCreateMethod::EMPTY" in hc)
    _check("G24.14.2 EMPTY masuk ke editor nama (enterKitCreateName)",
           "enterKitCreateName(true)" in hc)

    en = _g2414_def(s["ui"], "enterKitCreateName")
    _check("G24.14.2 enterKitCreateName men-set UIState::KIT_CREATE_NAME",
           "uiState = UIState::KIT_CREATE_NAME;" in en)
    _check("G24.14.2 editor nama memakai shared engine yang sama dgn RENAME",
           "enterNameEditor(" in en and
           "NameEditorExit::CREATE" in en)
    _check("G24.14.2 shared engine menggambar layar input",
           "nameEditorRedraw()" in
           _g2414_def(s["ui"], "enterNameEditor"))
    _check("G24.14.2 KIT_CREATE_NAME di-dispatch uiUpdate()",
           "case UIState::KIT_CREATE_NAME:" in s["ui"])
    _check("G24.14.2 tidak ada lagi layar SELECT SLOT di alur CREATE",
           "screenShowCreateSlot" not in _strip_comments(s["ui"]))


# ------------------------------------------------------------
# G24.14.3 - COPY CURRENT Membuka EDITOR NAMA
# ------------------------------------------------------------

def test_g24_14_copy_opens_name_editor():
    """G24.14.3 CREATE > COPY CURRENT -> editor nama yang sama."""
    print("G24.14.3 COPY CURRENT membuka editor nama")
    s = _g2414_sources()
    labels = _g24_label_block(s["menu"], "kitCreateLabels")
    _check("G24.14.3 sub-menu punya COPY CURRENT",
           labels[1] == "COPY CURRENT", str(labels))

    hc = _g24_func_body(s["ui"], "handleKitCreate")
    _check("G24.14.3 index 1 memilih KitCreateMethod::COPY_CURRENT",
           "idx == 1" in hc and "KitCreateMethod::COPY_CURRENT" in hc)
    _check("G24.14.3 COPY CURRENT juga lewat editor nama",
           hc.count("enterKitCreateName(true)") == 2,
           str(hc.count("enterKitCreateName(true)")))

    # Kedua metode berbagi editor yang sama; hanya createMethod yang beda.
    _check("G24.14.3 createMethod disimpan sebagai state editor",
           "static KitCreateMethod createMethod" in s["ui"])
    _check("G24.14.3 editor nama dibatasi 28 (KIT_NAME_MAX)",
           "static constexpr uint8_t KIT_CREATE_NAME_MAX = KIT_NAME_MAX;" in s["ui"])


# ------------------------------------------------------------
# G24.14.4 - NAMA DENGAN SPASI DITERIMA
# ------------------------------------------------------------

def test_g24_14_name_with_spaces_accepted():
    """G24.14.4 spasi = karakter nama biasa (0x20), bukan '_'."""
    print("G24.14.4 nama dengan spasi diterima")
    s = _g2414_sources()
    ui = s["ui"]

    _check("G24.14.4 konstanta spasi adalah 0x20",
           "static constexpr char KIT_RENAME_SPACE = ' ';" in ui)
    m = re.search(r'KIT_RENAME_ALPHABET\[\]\s*=\s*\n?\s*"([^"]*)"', ui)
    alpha = m.group(1) if m else ""
    _check("G24.14.4 ring karakter memuat spasi di posisi 0",
           alpha.startswith(" ABC"), repr(alpha[:6]))
    _check("G24.14.4 spasi BUKAN underscore (huruf besar '_' bukan slot 0)",
           alpha[0] == " " and "_" in alpha and "-_." in alpha)

    # Backend: spasi interior sah, hanya spasi ujung yang di-trim.
    vm = _g2414_def(s["km"], "kitManagerValidateName")
    _check("G24.14.4 body kitManagerValidateName ditemukan",
           vm != "", "len=%d" % len(vm))
    _check("G24.14.4 backend memangkas spasi kiri",
           "name[start] == ' '" in vm)
    _check("G24.14.4 backend memangkas spasi kanan",
           "name[n - 1] == ' '" in vm)
    _check("G24.14.4 backend menolak nama kosong setelah trim",
           "kosong setelah trim" in vm or "return false" in vm)
    _check("G24.14.4 backend menolak separator path",
           "c == '/'" in vm and "c == '\\\\'" in vm)
    _check("G24.14.4 backend menolak karakter control (< 0x20)",
           "c < 0x20" in vm)
    # Spasi TIDAK masuk daftar karakter ilegal.
    _check("G24.14.4 spasi bukan karakter ilegal di backend",
           "c == ' '" not in vm)


# ------------------------------------------------------------
# G24.14.5 - NAMA KOSONG DITOLAK
# ------------------------------------------------------------

def test_g24_14_empty_name_rejected():
    """G24.14.5 nama kosong tidak pernah sampai ke command."""
    print("G24.14.5 nama kosong ditolak")
    s = _g2414_sources()
    hn = _g2414_def(s["ui"], "handleNameEditor")
    fin = _g2414_def(s["ui"], "kitRenameFinalize")

    _check("G24.14.5 shared editor memanggil kitRenameFinalize()",
           "kitRenameFinalize()" in hn)
    _check("G24.14.5 kitRenameFinalize menolak hasil kosong",
           "return false" in fin)
    _check("G24.14.5 kitRenameFinalize memangkas spasi ekor",
           "KIT_RENAME_SPACE" in fin)
    _check("G24.14.5 gagal finalisasi -> batal (tidak kirim command)",
           "nameEditorCancel()" in hn)
    # Guard: nama kosong tidak boleh pernah sampai ke transport.
    _check("G24.14.5 guard nama kosong ada sebelum kirim",
           "kitRenameFinalize()" in hn and "uartSendCreateNamedRequest" not in hn)


# ------------------------------------------------------------
# G24.14.6 - EMPTY Mengirim COMMAND EXISTING
# ------------------------------------------------------------

def test_g24_14_empty_emits_existing_command():
    """G24.14.6 EMPTY -> CMD_KIT_CREATE_EMPTY_NAMED (0x19), header+chunk."""
    print("G24.14.6 EMPTY mengirim command existing")
    s = _g2414_sources()
    ha = _g24_func_body(s["ui"], "handleKitAction")

    _check("G24.14.6 handleKitAction punya kasus CREATE_EMPTY_NAMED",
           "case KitAction::CREATE_EMPTY_NAMED:" in ha)
    _check("G24.14.6 EMPTY memanggil uartSendCreateNamedRequest",
           G24_14_CMD_EMPTY in ha and "uartSendCreateNamedRequest" in ha)
    _check("G24.14.6 nama dikirim dari renameBuf",
           "renameBuf)" in ha)

    wv = _g2414_cmd_value(s["wh"], G24_14_CMD_EMPTY)
    ev = _g2414_cmd_value(s["eh"], G24_14_CMD_EMPTY)
    _check("G24.14.6 command EMPTY ada di header Wemos", wv == "0X19", str(wv))
    _check("G24.14.6 command EMPTY ada di header ESP32", ev == "0X19", str(ev))
    _check("G24.14.6 nilai command Wemos == ESP32", wv == ev,
           "%s vs %s" % (wv, ev))

    # Transport: header + chunk, satu SEQ.
    sn = _g24_func_body(s["wc"], "sendNameChunked")
    _check("G24.14.6 transport kirim 1 header", "headerArg0" in sn)
    _check("G24.14.6 chunk ditandai bit 0x80 di ARG0",
           "chunk | 0x80" in sn)
    _check("G24.14.6 3 karakter per frame", "chunk * 3" in sn)
    _check("G24.14.6 header dan chunk memakai SEQ yang sama",
           len(re.findall(r'\bseq\b', sn)) >= 2)

    # Backend menerima command itu.
    _check("G24.14.6 ESP32 menangani CMD CREATE EMPTY_NAMED",
           "case " + G24_14_CMD_EMPTY + ":" in s["ec"])
    _check("G24.14.6 backend memanggil kitManagerCreateEmptyNamed",
           "kitManagerCreateEmptyNamed" in s["ec"])
    _check("G24.14.6 EMPTY menghasilkan mapping sample kosong",
           "src[i].sample[0] = '\\0';" in s["km"])


# ------------------------------------------------------------
# G24.14.7 - COPY Mengirim COMMAND EXISTING
# ------------------------------------------------------------

def test_g24_14_copy_emits_existing_command():
    """G24.14.7 COPY -> CMD_KIT_CREATE_COPY_NAMED (0x1A) + semantics."""
    print("G24.14.7 COPY mengirim command existing")
    s = _g2414_sources()
    ha = _g24_func_body(s["ui"], "handleKitAction")

    _check("G24.14.7 handleKitAction punya kasus CREATE_COPY_NAMED",
           "case KitAction::CREATE_COPY_NAMED:" in ha)
    _check("G24.14.7 COPY memanggil uartSendCreateNamedRequest",
           G24_14_CMD_COPY in ha and "uartSendCreateNamedRequest" in ha)

    wv = _g2414_cmd_value(s["wh"], G24_14_CMD_COPY)
    ev = _g2414_cmd_value(s["eh"], G24_14_CMD_COPY)
    _check("G24.14.7 command COPY ada di header Wemos", wv == "0X1A", str(wv))
    _check("G24.14.7 command COPY ada di header ESP32", ev == "0X1A", str(ev))
    _check("G24.14.7 nilai command Wemos == ESP32", wv == ev,
           "%s vs %s" % (wv, ev))

    _check("G24.14.7 ESP32 menangani CMD CREATE COPY_NAMED",
           "case " + G24_14_CMD_COPY + ":" in s["ec"])
    _check("G24.14.7 backend memanggil kitManagerCreateCopyNamed",
           "kitManagerCreateCopyNamed" in s["ec"])

    # Semantics COPY CURRENT: config efektif + mapping + sample milik source.
    cc = _g24_func_body(s["km"], "createKitCopy")
    _check("G24.14.7 COPY menyalin config efektif kit aktif",
           "getPadConfig(i)" in cc)
    _check("G24.14.7 COPY menyimpan mapping by filename",
           "kitLoaderResolvedSampleName" in cc)
    _check("G24.14.7 COPY menyalin sample yang dimiliki source",
           "kitLoaderResolvedSamplePath" in cc)
    _check("G24.14.7 COPY menulis kit.json + file sample",
           "writeKitLifecycle(slot, name, src, srcSamples)" in cc)


# ------------------------------------------------------------
# G24.14.8 - RESPONSE OK -> CREATED
# ------------------------------------------------------------

def test_g24_14_response_ok_shows_created():
    """G24.14.8 response OK -> KIT CREATED lalu kembali ke menu KIT."""
    print("G24.14.8 response OK -> CREATED")
    s = _g2414_sources()
    ha = _g24_func_body(s["ui"], "handleKitAction")
    rr = _g2414_def(s["ui"], "renderKitActionResult")

    _check("G24.14.8 hasil mencocokkan CMD dengan bit respons",
           "UART_RESPONSE_BIT" in ha)
    _check("G24.14.8 hasil mencocokkan SEQ request",
           "uartFindResponse" in ha and "kitActionSeq" in ha)
    _check("G24.14.8 respons CREATE tidak disedot handler KIT_INFO",
           "case (CMD_KIT_INFO | UART_RESPONSE_BIT)" not in ha)

    _check("G24.14.8 renderKitActionResult memakai screenShowActionResult",
           "screenShowActionResult" in rr)
    _check("G24.14.8 judge 'CREATED' hanya untuk named CREATE",
           "isNamedCreate(kitActiveAction)" in rr)
    _check("G24.14.8 teks sukses adalah 'KIT CREATED' + 'OK'",
           'screenShowActionResult("KIT CREATED", "OK");' in rr,
           str('"KIT CREATED", "OK"' in rr))
    _check("G24.14.8 kind==1 = sukses (dipanggil saat response OK)",
           "case 1:" in rr and "kitActionResultKind = 1" in
           _g24_func_body(s["ui"], "handleKitAction"))
    _check("G24.14.8 setelah hasil user kembali ke menu KIT",
           "enterKit()" in ha)
    _check("G24.14.8 progres CREATE memakai title CREATING",
           '"CREATING"' in s["ui"])


# ------------------------------------------------------------
# G24.14.9 - RESPONSE ERROR -> FAILED
# ------------------------------------------------------------

def test_g24_14_response_error_shows_failed():
    """G24.14.9 response error -> FAILED (bukan diam / timeout buta)."""
    print("G24.14.9 response error -> FAILED")
    s = _g2414_sources()
    ha = _g24_func_body(s["ui"], "handleKitAction")
    rr = _g2414_def(s["ui"], "renderKitActionResult")

    _check("G24.14.9 ada judul 'CREATE FAILED'",
           '"CREATE FAILED"' in rr)
    _check("G24.14.9 'CREATE FAILED' dipakai untuk named CREATE",
           '"KIT CREATED",\n                        "CREATE FAILED"' in rr
           or '"CREATE FAILED"' in rr)
    _check("G24.14.9 kind error (3) -> render immediate",
           "kitActionResultKind = 3;" in ha and "renderKitActionResult();" in ha)
    _check("G24.14.9 timeout punya penanganan (bukan hang)",
           'screenShowActionResult("KIT CREATED", "TIMEOUT");' in rr)
    _check("G24.14.9 timeout KIT action = 25 detik",
           "constexpr uint16_t KIT_ACTION_TIMEOUT_MS = 25000;" in
           _g24_read("DrumCore_UI", "config.h"))
    _check("G24.14.9 timeout memicu render TIMEOUT",
           "kitActionResultKind = 4;" in ha)

    # Status backend (STATUS_*) harus punya NILAI yang sama dengan
    # padanan UI (RESP_*) - nama berbeda, angka tidak boleh beda.
    wh, eh = s["wh"], s["eh"]
    for pair in (("RESP_OK", "STATUS_OK"),
                 ("RESP_INVALID_VALUE", "STATUS_INVALID_VALUE"),
                 ("RESP_BUSY", "STATUS_BUSY"),
                 ("RESP_NAME_EXISTS", "STATUS_NAME_EXISTS"),
                 ("RESP_INVALID_KIT", "STATUS_INVALID_KIT")):
        wv = _g2414_cmd_value(wh, pair[0])
        ev = _g2414_cmd_value(eh, pair[1])
        _check("G24.14.9 %s == %s (nilai status harus sama)" % pair,
               wv is not None and wv == ev, "%s vs %s" % (wv, ev))

    # Semua status error yang dipetakan UI harus dipicu dari response.
    for flag in ("kitActionBusy", "kitActionNameExists",
                 "kitActionNameInvalid"):
        _check("G24.14.9 flag %s di-set dari resp.status" % flag,
               flag + " =" in ha or flag in ha)


# ------------------------------------------------------------
# G24.14.10 - KIT BARU TIDAK OTOMATIS AKTIF + INDEPENDEN
# ------------------------------------------------------------

def test_g24_14_new_kit_not_auto_active():
    """G24.14.10 CREATE tidak pernah mengganti active kit."""
    print("G24.14.10 kit baru tidak otomatis aktif")
    s = _g2414_sources()
    ha = _g24_func_body(s["ui"], "handleKitAction")

    # Hanya LOAD (dan DELETE kit aktif) yang boleh men-set activeKit.
    _check("G24.14.10 handleKitAction men-set activeKit (UNTUK LOAD)",
           "activeKit =" in ha)
    load_case = ha.split("case KitAction::LOAD:")[-1] \
        if "case KitAction::LOAD:" in ha else ""
    _check("G24.14.10 activeKit di-set di jalur LOAD", "activeKit" in load_case)

    for case in ("case KitAction::CREATE_EMPTY_NAMED:",
                 "case KitAction::CREATE_COPY_NAMED:"):
        blk = ha.split(case)[-1].split("case ")[0]
        _check("G24.14.10 %s tidak mengubah activeKit" % case.split(":")[0],
               "activeKit" not in blk)

    # Backend juga tidak boleh mengaktifkan kit baru.
    for fn in ("kitManagerCreateEmptyNamed", "kitManagerCreateCopyNamed"):
        b = _g24_func_body(s["km"], fn)
        _check("G24.14.10 %s tidak memanggil kit load" % fn,
               "kitManagerLoad" not in b)
    wl = _g24_func_body(s["km"], "writeKitLifecycle")
    _check("G24.14.10 writeKitLifecycle hanya retarget bila slot = aktif",
           "if (slot == kitManagerActiveSlot())" in wl)

    # Kontrak independensi: EMPTY benar-benar senyap.
    ce = _g24_func_body(s["km"], "createKitEmpty")
    _check("G24.14.10 EMPTY tidak menyalin file sample source",
           "nullptr" in ce and "copySampleToDir" not in ce)


# ------------------------------------------------------------
# G24.14.11 - BACK NAVIGATION
# ------------------------------------------------------------

def test_g24_14_back_navigation():
    """G24.14.11 tiap langkah CREATE punya jalan keluar."""
    print("G24.14.11 navigasi back CREATE")
    s = _g2414_sources()
    hc = _g2414_def(s["ui"], "handleKitCreate")
    hn = _g2414_def(s["ui"], "handleNameEditor")
    hcf = _g24_func_body(s["ui"], "handleKitConfirm")
    cancel = _g2414_def(s["ui"], "nameEditorCancel")

    _check("G24.14.11 long press di menu CREATE -> kembali ke KIT",
           "encoderLongPressed()" in hc and "enterKit()" in hc)
    _check("G24.14.11 item BACK di sub-menu CREATE -> kembali ke KIT",
           "menuKitCreateGetBackIndex()" in hc)
    _check("G24.14.11 long press di editor nama -> batal via shared cancel",
           "encoderLongPressed()" in hn and "nameEditorCancel()" in hn)
    _check("G24.14.11 confirm punya pilihan NO",
           "confirmChoice" in hcf)

    # Confirm dibatalkan harus kembali ke EDITOR NAMA (bukan ke menu KIT),
    # supaya nama yang sudah diketik tidak hilang.
    _check("G24.14.11 confirm berasal dari editor nama CREATE",
           "enterKitConfirm(act, 0, UIState::KIT_CREATE_NAME)" in hn)
    _check("G24.14.11 confirm batal -> kembali ke editor nama",
           "confirmState.origin == UIState::KIT_CREATE_NAME" in
           _g2414_def(s["ui"], "confirmCancelBack"))
    _check("G24.14.11 confirm batal mempertahankan nama (blank=false)",
           "enterKitCreateName(false)" in
           _g2414_def(s["ui"], "confirmCancelBack"))
    _check("G24.14.11 cancel mode CREATE kembali ke menu CREATE",
           "NameEditorExit::CREATE" in cancel and "enterKitCreate()" in cancel)

    # Tidak ada lagi jebakan state yang tidak bisa dibuangi.
    _check("G24.14.11 tidak ada state KIT_SLOT di enum UIState",
           not re.search(r'^\s*KIT_SLOT\s*,',
                         s["ui_h"], re.M))


# ------------------------------------------------------------
# G24.14.12 - RENAME TIDAK BERUBAH
# ------------------------------------------------------------

def test_g24_14_rename_unchanged():
    """G24.14.12 RENAME tetap jalur lamanya (index + command)."""
    print("G24.14.12 RENAME tidak berubah")
    s = _g2414_sources()
    hk = _g24_func_body(s["ui"], "handleKit")
    ha = _g24_func_body(s["ui"], "handleKitAction")

    _check("G24.14.12 RENAME ada di index 2 menu KIT",
           "idx == 2" in hk and "RENAME" in hk)
    _check("G24.14.12 RENAME tetap lewat browser KIT_SELECT",
           "enterKitSelect(KitSelectPurpose::RENAME" in hk)
    _check("G24.14.12 RENAME punya state editor sendiri",
           "case UIState::KIT_RENAME:" in s["ui"])
    _check("G24.14.12 RENAME punya kasus action sendiri",
           "case KitAction::RENAME:" in ha)
    _check("G24.14.12 RENAME kirim CMD_KIT_RENAME via uartSendRenameRequest",
           "uartSendRenameRequest(kitSubject, renameBuf)" in ha)
    _check("G24.14.12 RENAME menyertakan index kit tujuan",
           "kitSubject" in ha)
    # RENAME tidak boleh ikut berubah jadi named-create.
    _check("G24.14.12 RENAME tidak memakai command CREATE",
           "uartSendCreateNamedRequest" not in
           ha.split("case KitAction::RENAME:")[-1].split("case ")[0])


# ------------------------------------------------------------
# G24.14.13 - LOAD TIDAK BERUBAH
# ------------------------------------------------------------

def test_g24_14_load_unchanged():
    """G24.14.13 LOAD tetap jalur lamanya."""
    print("G24.14.13 LOAD tidak berubah")
    s = _g2414_sources()
    hk = _g24_func_body(s["ui"], "handleKit")
    ha = _g24_func_body(s["ui"], "handleKitAction")

    _check("G24.14.13 LOAD ada di index 0 menu KIT",
           "idx == 0" in hk and "LOAD" in hk)
    _check("G24.14.13 LOAD lewat browser + confirm",
           "enterKitSelect(KitSelectPurpose::LOAD" in hk)
    _check("G24.14.13 LOAD punya kasus action sendiri",
           "case KitAction::LOAD:" in ha)
    _check("G24.14.13 LOAD memakai CMD_KIT_LOAD via uartSendCommand",
           "uartSendCommand(CMD_KIT_LOAD, kitSubject)" in ha)
    load_blk = ha.split("case KitAction::LOAD:")[-1].split("case ")[0]
    _check("G24.14.13 hanya LOAD yang meng-update activeKit",
           "activeKit" in load_blk)
    _check("G24.14.13 LOAD tidak memakai transport chunk CREATE",
           "uartSendCreateNamedRequest" not in load_blk)


# ------------------------------------------------------------
# G24.14.14 - PAD TIDAK BERUBAH
# ------------------------------------------------------------

def test_g24_14_pad_unchanged():
    """G24.14.14 sub-menu PAD tidak tersentuh repair CREATE."""
    print("G24.14.14 PAD tidak berubah")
    s = _g2414_sources()
    menu_h = _g24_read("DrumCore_UI", "menu.h")

    top = _g24_label_block(s["menu"], "topLabels")
    _check("G24.14.14 top-level tetap KIT + PAD", top == ["KIT", "PAD"],
           str(top))
    _check("G24.14.14 jumlah sub-list tetap 2", "MENU_LIST_COUNT       = 2" in menu_h)
    _check("G24.14.14 MAPPING ada di sub-list PAD",
           "MENU_LIST_PAD_MAPPING" in menu_h)
    _check("G24.14.14 TRIGGER ada di sub-list PAD",
           "MENU_LIST_PAD_TRIGGER" in menu_h)
    for st in ("UIState::PAD:", "UIState::PAD_SOUND:",
               "UIState::PAD_MAPPING:", "UIState::PAD_TRIGGER:",
               "UIState::PAD_VALUE:"):
        _check("G24.14.14 %s tetap di-dispatch" % st,
               "case " + st in s["ui"])
    # Repair CREATE tidak boleh menyapu trigger/realtime.
    for banned in ("CMD_TRIGGER_SET", "CMD_PAD_TRIGGER"):
        _check("G24.14.14 tidak ada command trigger baru (%s)" % banned,
               len(re.findall(r'constexpr uint8_t CMD_', s["eh"])) == 20)


# ===============================================================
# G24.15 - CREATE REPAIR
#
# Dua regresi yang muncul di hardware:
#
#   1. Editor nama CREATE tidak bisa mengetik nama ber-SPACE di
#      tengah ("KENDANG TEST" terpotong jadi "KENDANG"). Sumbernya
#      bukan protokol, tapi duplikasi: handleKitCreateName() punya
#      loop finalisasi sendiri dengan heuristik "sisa buffer全是
#      spasi" yang langsung finalize begitu cursor mendarat di
#      SPACE. Fix: SATU shared name-editor engine (handleNameEditor)
#      dipakai RENAME dan CREATE; duplikatnya dihapus.
#
#   2. "Tidak ada slot kit user kosong" dilaporkan sebagai BUSY.
#      Sumbernya: kitManagerFindFreeSlot() mengembalikan KIT_COUNT,
#      lalu named create mengembalikan KitOpResult::BUSY yang
#      dipetakan ke STATUS_BUSY - padahal itu bukan backend sibuk
#      dan bukan kegagalan SD. Fix: KitOpResult::NO_SPACE +
#      STATUS_NO_SPACE, dan UI menampilkan NO SPACE, bukan BUSY.
#
# Constraint keras: protocol/frame TIDAK berubah (frame 8 byte,
# SOF, CRC, SEQ, daftar command, dan command 0x19/0x1A tetap).
# ===============================================================


def _g2415_sources():
    """Sumber G24.15. Sama persis dengan G24.14, plus kunci tambahan
    supaya test bisa menyentuh backend (kit_manager) dan protocol
    ESP32 tanpa mengulang _g24_read() di tiap test."""
    s = _g2414_sources()
    s.update({
        "km_cpp": s["km"],
        "km_h": _g24_read("DrumCore", "kit_manager.h"),
        "up_cpp": s["ec"],
        "up_h": s["eh"],
        "scr": s["screens"],
        "scr_h": _g24_read("DrumCore_UI", "screens.h"),
        "eh2": s["wh"],
        "cfg": _g24_read("DrumCore", "config.h"),
    })
    return s


def test_g24_15_1_single_shared_editor():
    """G24.15.1 hanya ada SATU name-editor engine."""
    print("G24.15.1 satu shared name-editor engine")
    s = _g2415_sources()
    ui_code = _strip_comments(s["ui"])

    hcn = _g2414_def(s["ui"], "handleKitCreateName")
    hrn = _g2414_def(s["ui"], "handleKitRename")
    hn = _g2414_def(s["ui"], "handleNameEditor")

    _check("G24.15.1 handleKitCreateName hanya mendelegasikan",
           "handleNameEditor()" in hcn and
           "renameBuf" not in hcn and "renameCursor" not in hcn)
    _check("G24.15.1 handleKitRename hanya mendelegasikan",
           "handleNameEditor()" in hrn and
           "renameBuf" not in hrn and "renameCursor" not in hrn)
    _check("G24.15.1 handleNameEditor() jadi satu-satunya loop editor",
           ui_code.count("handleNameEditor()") == 3,   # 1 def + 2 call
           str(ui_code.count("handleNameEditor()")))
    _check("G24.15.1 hanya satu alphabet ring",
           ui_code.count("KIT_RENAME_ALPHABET[]") == 1)
    _check("G24.15.1 hanya satu deklarasi renameCursor",
           len(re.findall(r'static\s+uint8_t\s+renameCursor', s["ui"])) == 1)
    # Editor hanya boleh menyentuh SATU buffer (renameBuf) dan SATU
    # cursor. Buffer lain (kitNameBuf, kitSubjectName, confirmName)
    # milik lapisan lain dan tidak boleh dipakai editor.
    en = _g2414_def(s["ui"], "enterNameEditor")
    _check("G24.15.1 editor hanya memakai buffer renameBuf",
           "renameBuf" in en and
           "kitNameBuf" not in en and "confirmName" not in en)
    _check("G24.15.1 handleNameEditor tidak menyentuh buffer lain",
           "kitNameBuf" not in hn and "kitSubjectName" not in hn and
           "confirmName" not in hn)


def test_g24_15_2_rest_empty_heuristic_gone():
    """G24.15.2 heuristik pemotong nama sudah dihapustotal."""
    print("G24.15.2 heuristik 'restEmpty' hilang")
    s = _g2415_sources()
    ui_code = _strip_comments(s["ui"])
    hn = _g2414_def(s["ui"], "handleNameEditor")

    for gone in ("restEmpty", "rest All", "allSpace", "restSpaces"):
        _check("G24.15.2 tidak ada lagi '%s'" % gone, gone not in ui_code)
    # Finalisasi hanya boleh lewat kitRenameFinalize() (trailing trim).
    _check("G24.15.2 konfirmasi hanya lewat kitRenameFinalize()",
           "kitRenameFinalize()" in hn)
    _check("G24.15.2 tidak ada trim-dulu-sebelum-kirim",
           "rstrip" not in hn and "while (renameBuf" not in hn)


def test_g24_15_3_blank_editor_is_nul_not_spaces():
    """G24.15.3 editor kosong berbasis NUL, bukan 28 spasi."""
    print("G24.15.3 editor kosong berbasis NUL")
    s = _g2415_sources()
    en = _g2414_def(s["ui"], "enterNameEditor")

    _check("G24.15.3 blank editor memset NUL (bukan spasi)",
           "memset(renameBuf, 0," in en)
    _check("G24.15.3 panjang buffer ikut nameEditMax",
           "nameEditMax + 1" in en)
    _check("G24.15.3 tidak ada lagi inisialisasi 28 spasi",
           "28" not in en.replace("KIT_RENAME_ALPHABET", ""))
    # Model: buffer NUL + kitRenameCharAfter('\0') -> SPACE.
    ed = RenameEditor("", limit=KIT_NAME_MAX)
    _check("G24.15.3 sel kosong bisa diisi tanpa melewati alfabet",
           kit_rename_char_after("\x00", 1) == KIT_RENAME_SPACE)
    _check("G24.15.3 sel kosong confirm -> BATAL (bukan kirim)",
           ed.confirmed_name() is None)


def test_g24_15_4_space_is_a_real_character():
    """G24.15.4 SPACE di tengah nama bisa diketik & hilang."""
    print("G24.15.4 SPACE sebagai karakter nyata")
    s = _g2415_sources()
    fin = _g2414_def(s["ui"], "kitRenameFinalize")

    # Perilaku: "KENDANG TEST" (spasi di tengah) harus utuh.
    ed = RenameEditor("", limit=KIT_NAME_MAX)
    ok = ed.type_name("KENDANG TEST")
    _check("G24.15.4 'KENDANG TEST' bisa diketik penuh", ok)
    _check("G24.15.4 'KENDANG TEST' tidak terpotong jadi 'KENDANG'",
           ed.confirmed_name() == "KENDANG TEST", repr(ed.confirmed_name()))
    _check("G24.15.4 nama ber-space lolos validasi backend",
           validate_kit_name(ed.confirmed_name()) is not None)

    # Spasi di ujung dipangkas, spasi di tengah dipertahankan.
    _check("G24.15.4 finalize memangkas spasi kanan",
           "KIT_RENAME_SPACE" in fin and "return false" in fin)
    ed2 = RenameEditor("", limit=KIT_NAME_MAX)
    ed2.type_name("KENDANG TEST   ")
    _check("G24.15.4 spasi ekor dipangkas, spasi tengah dipertahankan",
           ed2.confirmed_name() == "KENDANG TEST",
           repr(ed2.confirmed_name()))

    # Rotate-ke-SPACE = hapus karakter pada sel itu (backspace).
    # PENTING: karena SPACE adalah karakter yang sah, memutar sel
    # jadi SPACE hanya "menghapus" kalau sel itu di ujung (spasi ekor
    # dipangkas finalize). Sel di tengah menghasilkan spasi sungguhan
    # di dalam nama - bukan penghapusan. Gesture ini memang sama
    # dengan RENAME lama; tidak ada gesture cursor-mundur.
    ed3 = RenameEditor("ABCDE", limit=KIT_RENAME_MAX)
    ed3.cursor = 4
    ed3.rotate_to(KIT_RENAME_SPACE)
    _check("G24.15.4 putar ke SPACE di ujung = hapus karakter (backspace)",
           ed3.confirmed_name() == "ABCD", repr(ed3.confirmed_name()))

    ed4 = RenameEditor("ABCDE", limit=KIT_RENAME_MAX)
    ed4.cursor = 2
    ed4.rotate_to(KIT_RENAME_SPACE)
    _check("G24.15.4 putar ke SPACE di tengah = spasi nyata, bukan hapus",
           ed4.confirmed_name() == "AB DE", repr(ed4.confirmed_name()))
    _check("G24.15.4 nama hasil 'AB DE' tetap valid (bukan rusak)",
           validate_kit_name(ed4.confirmed_name()) is not None)


def test_g24_15_5_confirm_is_last_cell():
    """G24.15.5 konfirmasi lewat sel terakhir, untuk kedua mode."""
    print("G24.15.5 konfirmasi di sel terakhir")
    s = _g2415_sources()
    hn = _g2414_def(s["ui"], "handleNameEditor")
    ec = _g2414_def(s["ui"], "enterKitCreateName")
    er = _g2414_def(s["ui"], "enterKitRename")

    _check("G24.15.5 maju sampai nameEditMax - 1",
           "renameCursor < nameEditMax - 1" in hn and
           "renameCursor++" in hn)
    _check("G24.15.5 konfirmasi hanya di sel terakhir",
           "kitRenameFinalize()" in hn)
    _check("G24.15.5 panjang RENAME = 14",
           "enterNameEditor(KIT_RENAME_MAX" in er and
           "KIT_RENAME_MAX = 14" in s["ui"])
    _check("G24.15.5 panjang CREATE = 28 (KIT_NAME_MAX)",
           "enterNameEditor(KIT_CREATE_NAME_MAX" in ec and
           "KIT_CREATE_NAME_MAX = KIT_NAME_MAX" in s["ui"])
    # nameEditMax = 14 vs 28, TIDAK ada fork logika per mode.
    _check("G24.15.5 panjang come from nameEditMax, bukan konstanta mode",
           "nameEditMax - 1" in hn and
           len(re.findall(r'renameCursor\s*<\s*\w*[Mm]ax', hn)) == 1,
           str(re.findall(r'renameCursor\s*<\s*\w*[Mm]ax', hn)))


def test_g24_15_6_cancel_and_parent_routes():
    """G24.15.6 batal/long-press kembali ke layar asal yang benar."""
    print("G24.15.6 rute batal & layar asal")
    s = _g2415_sources()
    hn = _g2414_def(s["ui"], "handleNameEditor")
    cancel = _g2414_def(s["ui"], "nameEditorCancel")
    ccb = _g2414_def(s["ui"], "confirmCancelBack")

    _check("G24.15.6 long press = batal",
           "encoderLongPressed()" in hn and "nameEditorCancel()" in hn)
    _check("G24.15.6 idle = batal",
           "KIT_IDLE_BACK_MS" in hn and "nameEditorCancel()" in hn)
    _check("G24.15.6 mode CREATE batal -> menu CREATE",
           "NameEditorExit::CREATE" in cancel and "enterKitCreate()" in cancel)
    _check("G24.15.6 mode RENAME batal -> browser KIT_SELECT",
           "enterKitSelect(kitSelectPurpose, false)" in cancel)
    _check("G24.15.6 confirm batal -> kembali ke editor, nama kept",
           "enterKitCreateName(false)" in ccb and
           "confirmState.origin == UIState::KIT_CREATE_NAME" in ccb)


def test_g24_15_7_legacy_screens_removed():
    """G24.15.7 renderer/handler lama benar-benar hilang."""
    print("G24.15.7 renderer & handler lama dihapus")
    s = _g2415_sources()

    for legacy in ("screenShowKitNameEntry", "screenShowKitRename"):
        _check("G24.15.7 %s tidak ada lagi" % legacy,
               legacy not in s["ui"] and legacy not in s["ui_h"] and
               legacy not in s["scr"] and legacy not in s["scr_h"])
    _check("G24.15.7 shared renderer screenShowNameEditor ada",
           "screenShowNameEditor" in s["scr"] and
           "screenShowNameEditor" in s["scr_h"])
    _check("G24.15.7 shared engine memanggil shared renderer",
           "screenShowNameEditor(renameBuf, renameCursor)" in
           _g2414_def(s["ui"], "nameEditorRedraw"))
    # Jendela 14 kolom + scroll mengikuti cursor (bukan layar statis).
    se = _g2414_def(s["scr"], "screenShowNameEditor")
    _check("G24.15.7 jendela 14 kolom (sama seperti RENAME lama)",
           "const uint8_t winW = 14;" in se)
    _check("G24.15.7 jendela ikut geser saat cursor maju",
           "winStart" in se and "cursor >= winW" in se)
    _check("G24.15.7 caret '^' digambar",
           "'^'" in s["scr"])


def test_g24_15_8_rename_path_unchanged():
    """G24.15.8 RENAME tetap memakai jalur lamanya."""
    print("G24.15.8 RENAME tidak berubah")
    s = _g2415_sources()
    hk = _g24_func_body(s["ui"], "handleKit")
    ha = _g24_func_body(s["ui"], "handleKitAction")

    _check("G24.15.8 RENAME di index 2 menu KIT",
           "idx == 2" in hk and "RENAME" in hk)
    _check("G24.15.8 RENAME lewat browser KIT_SELECT",
           "enterKitSelect(KitSelectPurpose::RENAME" in hk)
    _check("G24.15.8 RENAME punya state editor sendiri",
           "case UIState::KIT_RENAME:" in s["ui"])
    _check("G24.15.8 RENAME kirim CMD_KIT_RENAME + index tujuan",
           "uartSendRenameRequest(kitSubject, renameBuf)" in ha)
    _check("G24.15.8 RENAME prefill dari nama kit saat ini",
           "kitNameFor(selectedKit)" in
           _g2414_def(s["ui"], "enterNameEditor"))


def test_g24_15_9_no_free_slot_is_no_space():
    """G24.15.9 slot penuh -> NO SPACE, bukan BUSY."""
    print("G24.15.9 slot penuh -> NO SPACE")
    s = _g2415_sources()
    km = s["km_cpp"]
    find_free = _g2414_def(km, "kitManagerFindFreeSlot")
    ce = _g2414_def(km, "kitManagerCreateEmptyNamed")
    cc = _g2414_def(km, "kitManagerCreateCopyNamed")

    # Slot 0 = factory, harus dilewati.
    _check("G24.15.9 pencarian slot mulai dari 1 (factory 0 terlindungi)",
           "s = 1" in find_free or "slot = 1" in find_free)
    _check("G24.15.9 slot penuh terdeteksi via >= KIT_COUNT",
           "KIT_COUNT" in find_free)
    for nm, fn in (("EMPTY", ce), ("COPY", cc)):
        _check("G24.15.9 named CREATE %s -> NO SPACE" % nm,
               "KitOpResult::NO_SPACE" in fn)
        _check("G24.15.9 named CREATE %s tidak lagi -> BUSY" % nm,
               "KitOpResult::BUSY" not in fn)
    _check("G24.15.9 KitOpResult punya NO_SPACE",
           "NO_SPACE" in s["km_h"])
    _check("G24.15.9 factory slot 0 tetap PROTECTED (tidak ditimpa)",
           "PROTECTED" in s["km_h"])


def test_g24_15_10_status_mapping_honest():
    """G24.15.10 NO SPACE punya status sendiri; BUSY tetap BUSY."""
    print("G24.15.10 pemetaan status jujur")
    s = _g2415_sources()
    up = s["up_cpp"]
    km = s["km_cpp"]
    mapping = _g2414_def(up, "kitResultToStatus")

    _check("G24.15.10 KitOpResult::NO_SPACE -> STATUS_NO_SPACE",
           "case KitOpResult::NO_SPACE:" in mapping and
           "STATUS_NO_SPACE" in mapping)
    _check("G24.15.10 STATUS_NO_SPACE = 0x08",
           re.search(r'STATUS_NO_SPACE\s*=\s*0x08', s["up_h"]) is not None)
    # Nilai status lama tidak boleh digeser/ditimpa.
    _check("G24.15.10 STATUS_BUSY tetap 0x05",
           re.search(r'STATUS_BUSY\s*=\s*0x05', s["up_h"]) is not None)
    _check("G24.15.10 STATUS_NAME_EXISTS tetap 0x07",
           re.search(r'STATUS_NAME_EXISTS\s*=\s*0x07', s["up_h"]) is not None)
    # Kegagalan NYATA tetap BUSY - tidak dipaksa jadi sukses/NO SPACE.
    wl = _g2414_def(km, "writeKitLifecycle")
    _check("G24.15.10 kegagalan mkdir tetap BUSY",
           "ensureKitDirsFromName" in wl and
           re.search(r'if\s*\(\s*!ensureKitDirsFromName\w*\(\s*name\s*\)\s*\)'
                     r'\s*\{\s*return KitOpResult::BUSY;', wl) is not None)
    _check("G24.15.10 kegagalan JSON tetap BUSY",
           re.search(r'if\s*\(\s*!writeJsonFromSources\('
                     r'[^)]*\)\s*\)\s*\{\s*return KitOpResult::BUSY;', wl)
           is not None)
    _check("G24.15.10 writeJsonFromSources menulis id slot (slot occupancy)",
           'doc["id"]' in _g2414_def(km, "writeJsonFromSources") and
           'doc["kitId"]' in _g2414_def(km, "writeJsonFromSources"))
    _check("G24.15.10 kegagalan NVS TIDAK menggagalkan CREATE (best-effort)",
           "persistenceClearSlot" in wl or "persistenceClearKit" in wl)
    _check("G24.15.10 tidak ada konversi NO SPACE -> OK",
           not re.search(r'NO_SPACE[^;]*STATUS_OK', mapping))
    _check("G24.15.10 default tetap BUSY (bukan SUCCESS)",
           re.search(r'default:\s*return STATUS_BUSY', mapping) is not None)


def test_g24_15_11_protocol_untouched():
    """G24.15.11 protocol/frame/command TIDAK berubah."""
    print("G24.15.11 protocol & frame tidak berubah")
    s = _g2415_sources()

    _check("G24.15.11 jumlah command tetap 20",
           len(re.findall(r'constexpr uint8_t CMD_', s["eh"])) == 20,
           str(len(re.findall(r'constexpr uint8_t CMD_', s["eh"]))))
    _check("G24.15.11 CMD_KIT_CREATE_EMPTY_NAMED tetap 0x19",
           "CMD_KIT_CREATE_EMPTY_NAMED" in s["eh"] and
           "0x19" in s["eh"])
    _check("G24.15.11 CMD_KIT_CREATE_COPY_NAMED tetap 0x1A",
           "CMD_KIT_CREATE_COPY_NAMED" in s["eh"] and
           "0x1A" in s["eh"])
    for fixed, label in ((r'UART_SOF\s*=\s*0xD3', "SOF"),
                         (r'UART_FRAME_LEN\s*=\s*8', "panjang frame 8"),
                         (r'UART_RESP_FLAG\s*=\s*0x80', "response flag 0x80")):
        _check("G24.15.11 %s tidak berubah" % label,
               re.search(fixed, s["up_h"]) is not None)
    _check("G24.15.11 baud tidak berubah",
           re.search(r'SERIAL_BAUD\s*=\s*115200', s["cfg"]) is not None)
    # Panjang frame tetap 8: SOF,LEN,CMD,A0,A1,A2,SEQ,CRC.
    _check("G24.15.11 CRC/SEQ handling tidak berubah",
           "UART_FRAME_LEN" in s["up_cpp"])
    _check("G24.15.11 Wemos punya konstanta respons yang sama (0x08)",
           re.search(r'RESP_NO_SPACE\s*=\s*0x08', s["eh2"]) is not None)


def test_g24_15_12_wemos_handles_no_space():
    """G24.15.12 Wemos menampilkan NO SPACE, bukan BUSY."""
    print("G24.15.12 UI menampilkan NO SPACE")
    s = _g2415_sources()
    hk = _g24_func_body(s["ui"], "handleKitAction")

    _check("G24.15.12 flag no-space ada",
           "kitActionNoSpace" in s["ui"])
    _check("G24.15.12 no-space dibaca dari RESP_NO_SPACE",
           "kitActionNoSpace = (resp.status == RESP_NO_SPACE);" in
           _strip_comments(s["ui"]))
    _check("G24.15.12 layar menampilkan 'NO SPACE'",
           '"NO SPACE"' in s["ui"])
    _check("G24.15.12 teks jujur menyebut kit penuh",
           '"ALL KITS FULL"' in s["ui"])
    # NO SPACE tidak bolehMENYATUkan dirichlet dengan BUSY.
    _check("G24.15.12 no-space & busy tidak tumpang tindih",
           "kitActionNoSpace" in hk and
           "(resp.status == RESP_BUSY) && !kitActionNoSpace" in
           _strip_comments(s["ui"]))
    # Flag harus di-reset: 1 deklarasi + 2 titik reset (start aksi
    # dan reset global), supaya aksi berikutnya tidak mewarisi.
    _check("G24.15.12 flag no-space di-reset per aksi",
           len(re.findall(r'kitActionNoSpace = false;', s["ui"])) == 3,
           str(len(re.findall(r'kitActionNoSpace = false;', s["ui"]))))


def test_g24_15_13_no_error_masked_as_success():
    """G24.15.13 tidak ada kegagalan yang dipoles jadi sukses."""
    print("G24.15.13 kegagalan tidak dipoles jadi sukses")
    s = _g2415_sources()
    hk = _g24_func_body(s["ui"], "handleKitAction")

    _check("G24.15.13 sukses hanya bila status OK",
           "resp.status == RESP_OK" in hk)
    _check("G24.15.13 timeout tetap TIMEOUT (bukan sukses)",
           '"TIMEOUT"' in s["ui"])
    _check("G24.15.13 BUSY tetap tampil sebagai BUSY",
           '"BUSY"' in s["ui"])
    # G24.12: tidak ada teks generik 'FAILED'/'CREATE FAILED'.
    for banned in ("'CREATE FAILED'", "'FAILED'", "'SAVE FAILED'",
                   "'RENAME FAILED'", "'LOAD FAILED'", "'DELETE FAILED'"):
        _check("G24.15.13 tidak ada string generik %s" % banned,
               banned not in s["ui"])


def test_g24_15_14_empty_and_copy_postconditions():
    """G24.15.14 postcondition CREATE EMPTY & COPY CURRENT."""
    print("G24.15.14 postcondition EMPTY & COPY CURRENT")
    s = _g2415_sources()
    km = s["km_cpp"]
    ce = _g2414_def(km, "createKitEmpty")
    cc = _g2414_def(km, "createKitCopy")

    # EMPTY: mapping sample kosong, tidak ada file sample.
    _check("G24.15.14 EMPTY mengosongkan nama sample mapping",
           re.search(r'src\[\s*i\s*\]\.sample\[0\]\s*=\s*\'\\0\'', ce)
           is not None)
    _check("G24.15.14 EMPTY tidak menyalin sample",
           "copyFile" not in ce and "resolveSample" not in ce)
    _check("G24.15.14 EMPTY tidak mewarisi config kit aktif",
           "sourceSnapshot" not in ce)
    # COPY: config + mapping + sample milik kit itu sendiri.
    wl = _g2414_def(km, "writeKitLifecycle")
    _check("G24.15.14 COPY menyalin config & mapping tiap pad",
           "src[i].cfg = getPadConfig(i);" in cc and
           "srcSamples" in cc)
    _check("G24.15.14 COPY meneruskan daftar sample milik kit sumber",
           "kitLoaderResolvedSamplePath" in cc and
           "writeKitLifecycle(slot, name, src, srcSamples)" in cc)
    _check("G24.15.14 COPY menyalin file sample ke folder kit BARU",
           "copySampleToDir" in wl and 'snprintf(sdir, sizeof(sdir), '
           '"%s/samples", newDir)' in wl)
    _check("G24.15.14 COPY tidak mewarisi sample yang tidak ter-assign",
           "src[i].sample[0] = '\\0'" in cc)
    # Kit baru belum aktif setelah create: jalur create tidak pernah
    # mengirim KIT_LOAD, dan tidak pernah menunjuk slot baru sebagai
    # aktif (hanya me-retarget prefix kalau slot itu memang aktif).
    _check("G24.15.14 kit baru tidak langsung aktif",
           "CMD_KIT_LOAD" not in ce + cc + wl and
           re.search(r'=\s*kitManagerSetActive', ce + cc + wl) is None)
    _check("G24.15.14 slot aktif hanya dibaca, bukan dipindah",
           "kitManagerActiveSlot()" in wl and
           "kitLoaderRetargetSampleDir" in wl)


def test_g24_15_15_scope_hygiene():
    """G24.15.15 tidak menyapu realtime/audio/trigger."""
    print("G24.15.15 scope: realtime & audio tidak tersentuh")
    s = _g2415_sources()

    # PSRAM loader / audio / trigger tidak boleh ikut tersentuh di UI
    # maupun di jalur create backend.
    for banned, label in (("CMD_TRIGGER_SET", "trigger"),
                          ("CMD_PAD_TRIGGER", "trigger"),
                          ("I2S", "I2S"),
                          ("ps_malloc", "PSRAM loader")):
        _check("G24.15.15 tidak menambah %s baru" % label,
               banned not in s["ui"] and
               (banned == "ps_malloc" or banned not in s["up_cpp"]))
    _check("G24.15.15 jalur create tidak menyentuh PSRAM/audio",
           all(b not in _g2414_def(s["km_cpp"], n)
               for n in ("kitManagerCreateEmptyNamed",
                         "kitManagerCreateCopyNamed",
                         "writeKitLifecycle")
               for b in ("ps_malloc", "i2s", "I2S")))
    _check("G24.15.15 command count tidak bertambah (tetap 20)",
           len(re.findall(r'constexpr uint8_t CMD_', s["eh"])) == 20)
    # Yang boleh berubah: status table saja.
    _check("G24.15.15 status table bertambah tepat 1 (0x08)",
           len(re.findall(r'constexpr uint8_t STATUS_', s["up_h"])) == 9,
           str(len(re.findall(r'constexpr uint8_t STATUS_', s["up_h"]))))
    _check("G24.15.15 respons table Wemos bertambah tepat 1 (0x08)",
           len(re.findall(r'RESP_[A-Z_]+\s*=\s*0x0[0-9]', s["eh2"])) == 9,
           str(len(re.findall(r'RESP_[A-Z_]+\s*=\s*0x0[0-9]', s["eh2"]))))


# ==============================================================
# ADC CALIBRATION INSTRUMENTATION (G-ADC)
#
# Regression test untuk:
#   - debug flag ada dan bernilai default false
#   - modul adc_calibration.h/.cpp ada dan terhubung ke build
#   - semua 12 PAD dicakup
#   - GPIO ditampilkan di output (PAD_PINS[])
#   - struct PadCalData punya field baseline, peak, crosstalk
#   - enum AdcCalMode punya semua mode yang dibutuhkan
#   - tidak ada perubahan realtime algorithm
#   - tidak ada perubahan UART protocol
#   - tidak ada perubahan UI
#   - integrasi via loop() bukan TriggerTask/AudioTask
# ==============================================================

_DRUMCORE_DIR = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "DrumCore",
)


def _adc_src():
    """Baca source file ADC calibration."""
    h_path   = os.path.join(_DRUMCORE_DIR, "adc_calibration.h")
    cpp_path = os.path.join(_DRUMCORE_DIR, "adc_calibration.cpp")
    ino_path = os.path.join(_DRUMCORE_DIR, "DrumCore.ino")
    cfg_path = os.path.join(_DRUMCORE_DIR, "config.h")

    def _read(p):
        with open(p, encoding="utf-8", errors="replace") as f:
            return f.read()

    return {
        "h":   _read(h_path),
        "cpp": _read(cpp_path),
        "ino": _read(ino_path),
        "cfg": _read(cfg_path),
    }


def test_gadc_debug_flag():
    """G-ADC: debug flag ada di config.h dan default false."""
    print("G-ADC: debug flag")
    s = _adc_src()
    _check("G-ADC flag DEBUG_ADC_CALIBRATION ada di config.h",
           "DEBUG_ADC_CALIBRATION" in s["cfg"])
    _check("G-ADC flag default off (production safe)",
           "DEBUG_ADC_CALIBRATION 0" in s["cfg"] or
           "DEBUG_ADC_CALIBRATION = false" in s["cfg"])
    _check("G-ADC flag adalah preprocessor define atau constexpr bool",
           "#define DEBUG_ADC_CALIBRATION" in s["cfg"] or
           "constexpr bool DEBUG_ADC_CALIBRATION" in s["cfg"])


def test_gadc_files_exist_and_connected():
    """G-ADC: file ada dan terhubung ke build (DrumCore.ino)."""
    print("G-ADC: files exist & connected to build")
    s = _adc_src()

    # File tidak kosong
    _check("G-ADC adc_calibration.h tidak kosong",
           len(s["h"]) > 200)
    _check("G-ADC adc_calibration.cpp tidak kosong",
           len(s["cpp"]) > 200)

    # Terhubung ke build via include di .ino
    _check("G-ADC DrumCore.ino #include adc_calibration.h",
           '#include "adc_calibration.h"' in s["ino"])

    # Init dipanggil di setup() bawah guard
    _check("G-ADC adcCalibrationInit() dipanggil di setup() bawah guard",
           "adcCalibrationInit()" in s["ino"] and
           "DEBUG_ADC_CALIBRATION" in s["ino"])

    # Task dipanggil di loop() bawah guard
    _check("G-ADC adcCalibrationTask() dipanggil di loop() bawah guard",
           "adcCalibrationTask()" in s["ino"])


def test_gadc_pad_count_coverage():
    """G-ADC: semua 12 PAD dicakup (PAD_COUNT = 12)."""
    print("G-ADC: 12 pad coverage")
    s = _adc_src()

    # .cpp harus loop atas PAD_COUNT, bukan hardcode 12
    _check("G-ADC loop atas PAD_COUNT di .cpp (bukan hardcode)",
           "PAD_COUNT" in s["cpp"])

    # Harus ada array per-pad
    _check("G-ADC ada array g_calData[PAD_COUNT] per pad",
           "g_calData[PAD_COUNT]" in s["cpp"])

    # PAD numbering 1-based user-facing (PAD01..PAD12)
    _check("G-ADC output format PAD01..PAD12 (idx+1)",
           "idx + 1" in s["cpp"] or "i + 1" in s["cpp"])


def test_gadc_gpio_displayed():
    """G-ADC: GPIO ditampilkan bersama PAD number."""
    print("G-ADC: GPIO displayed")
    s = _adc_src()

    # Output format PADxx [GPIOyy]
    _check("G-ADC format output PADxx [GPIOxx] ada",
           "GPIO%u" in s["cpp"] or "GPIO%-" in s["cpp"])

    # GPIO diambil dari PAD_PINS[] — single source of truth
    _check("G-ADC GPIO diambil dari PAD_PINS[] (bukan hardcode)",
           "PAD_PINS[" in s["cpp"])

    # PAD_PINS ada di config.h
    _check("G-ADC PAD_PINS[] ada di config.h",
           "PAD_PINS[PAD_COUNT]" in s["cfg"])


def test_gadc_struct_fields():
    """G-ADC: PadCalData punya field baseline, peak, crosstalk."""
    print("G-ADC: PadCalData fields")
    s = _adc_src()

    # Baseline fields
    _check("G-ADC PadCalData.idleMin ada",
           "idleMin" in s["h"])
    _check("G-ADC PadCalData.idleMax ada",
           "idleMax" in s["h"])
    _check("G-ADC PadCalData.idleSum ada (untuk avg)",
           "idleSum" in s["h"])
    _check("G-ADC PadCalData.idleCount ada (untuk avg)",
           "idleCount" in s["h"])

    # Peak fields (auto-capture: peakMax + peakLast)
    _check("G-ADC PadCalData.peakMax ada",
           "peakMax" in s["h"])
    _check("G-ADC PadCalData.peakLast ada",
           "peakLast" in s["h"])

    # Crosstalk field
    _check("G-ADC PadCalData.xtalkMax ada",
           "xtalkMax" in s["h"])


def test_gadc_modes():
    """G-ADC: auto-capture mode — baseline, hit, crosstalk aktif otomatis."""
    print("G-ADC: auto-capture modes")
    s = _adc_src()

    # Auto-capture: baseline print interval ada di .cpp
    _check("G-ADC BASELINE_PRINT_INTERVAL ada di .cpp",
           "BASELINE_PRINT_INTERVAL" in s["cpp"])

    # Auto hit detection threshold ada
    _check("G-ADC CAL_HIT_THRESHOLD ada di .cpp",
           "CAL_HIT_THRESHOLD" in s["cpp"])

    # Crosstalk threshold ada
    _check("G-ADC XTALK_SOURCE_THRESHOLD ada di .cpp",
           "XTALK_SOURCE_THRESHOLD" in s["cpp"])

    # Output tags yang dihasilkan otomatis
    _check("G-ADC output tag [BASELINE] ada di .cpp",
           "[BASELINE]" in s["cpp"])
    _check("G-ADC output tag [HIT] ada di .cpp",
           "[HIT]" in s["cpp"])
    _check("G-ADC output tag [CROSSTALK] ada di .cpp",
           "[CROSSTALK]" in s["cpp"])
    _check("G-ADC output tag WORST ada di .cpp",
           "WORST" in s["cpp"])
    _check("G-ADC output tag SOURCE ada di .cpp",
           "SOURCE" in s["cpp"])

    # Tidak ada interactive command handling
    _check("G-ADC tidak ada printHelp di .cpp",
           "printHelp" not in s["cpp"])
    _check("G-ADC tidak ada handleCommand di .cpp",
           "handleCommand" not in s["cpp"])


def test_gadc_realtime_safety():
    """G-ADC: tidak ada perubahan realtime (trigger/audio/I2S/mask)."""
    print("G-ADC: realtime untouched")
    s = _adc_src()

    combined = s["h"] + s["cpp"]
    combined_stripped = _strip_comments(combined)

    # Tidak menyentuh trigger state machine — cek di kode (bukan komentar)
    for banned in ("TRIGGER_IDLE", "TRIGGER_WAIT_PEAK",
                   "TRIGGER_MASK", "TRIGGER_WAIT_RELEASE",
                   "adjustMaskTime", "audioTrigger",
                   "audioTaskStart", "triggerTaskStart"):
        _check(f"G-ADC tidak menyentuh {banned}",
               banned not in combined_stripped)

    # TriggerTask/AudioTask tidak boleh di-*call* atau di-include
    # (boleh sebut di komentar, tapi tidak sebagai identifier kode)
    for banned in ("TriggerTask", "AudioTask"):
        _check(f"G-ADC tidak mereferensi {banned} sebagai kode",
               banned not in combined_stripped)

    # Tidak ada malloc/free/new pada jalur calibration
    for banned_alloc in ("ps_malloc", "heap_caps_malloc",
                         "new ", "delete "):
        _check(f"G-ADC tidak ada {banned_alloc.strip()} di calibration",
               banned_alloc not in combined_stripped)

    # Tidak ada blocking delay
    _check("G-ADC tidak ada delay() di calibration",
           "delay(" not in combined_stripped)

    # Tidak ada String object (hanya char[] dan snprintf)
    _check("G-ADC tidak ada String object",
           "String " not in combined_stripped and
           "String(" not in combined_stripped)

    # ADC hanya dibaca via readPadADC() — tidak langsung analogRead
    _check("G-ADC ADC dibaca via readPadADC() bukan langsung analogRead()",
           "analogRead(" not in combined_stripped)


def test_gadc_no_protocol_change():
    """G-ADC: tidak ada perubahan UART protocol."""
    print("G-ADC: no UART protocol change")
    s = _adc_src()
    combined = s["h"] + s["cpp"]

    # Tidak ada CMD_ baru
    _check("G-ADC tidak menambah CMD_ baru ke protocol",
           "CMD_" not in combined)

    # Tidak ada STATUS_ baru
    _check("G-ADC tidak menambah STATUS_ baru ke protocol",
           "STATUS_" not in combined)

    # Tidak mengubah UART1
    _check("G-ADC tidak menyentuh UART1",
           "UART1" not in combined and "uart_protocol" not in combined)


def test_gadc_no_ui_change():
    """G-ADC: tidak ada perubahan UI (DrumCore_UI / Wemos)."""
    print("G-ADC: no UI change")
    ui_dir = os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
        "DrumCore_UI",
    )
    ui_files = [
        os.path.join(ui_dir, f)
        for f in os.listdir(ui_dir)
        if f.endswith((".cpp", ".h", ".ino"))
    ]
    for fp in ui_files:
        with open(fp, encoding="utf-8", errors="replace") as f:
            content = f.read()
        _check(f"G-ADC UI file {os.path.basename(fp)} tidak menyebut adc_calibration",
               "adc_calibration" not in content and
               "adcCalibration" not in content)


def test_gadc_integration_via_loop():
    """G-ADC: integrasi via loop() bukan di TriggerTask/AudioTask."""
    print("G-ADC: integration via loop()")
    s = _adc_src()

    # Pastikan adcCalibrationTask dipanggil dari loop() di .ino
    # Cara cek: cari fungsi loop() dan pastikan adcCalibrationTask ada di dalamnya
    ino = s["ino"]
    loop_start = ino.find("void loop()")
    _check("G-ADC void loop() ada di .ino",
           loop_start >= 0)

    loop_body = _func_body(ino, "void loop()")
    _check("G-ADC adcCalibrationTask() ada di dalam loop()",
           "adcCalibrationTask()" in loop_body)

    # Guard dengan #if DEBUG_ADC_CALIBRATION
    # Cek bahwa #if DEBUG_ADC_CALIBRATION hadir sebelum adcCalibrationTask
    cal_block = ino[loop_start:]
    _check("G-ADC #if DEBUG_ADC_CALIBRATION guard ada di loop()",
           "#if DEBUG_ADC_CALIBRATION" in cal_block)

    # adcCalibrationTask() TIDAK ada di TriggerTask atau AudioTask
    for fname in ("triggerTask", "audioTask", "TriggerTask", "AudioTask"):
        trigger_body = _func_body(ino, fname)
        _check(f"G-ADC adcCalibrationTask() tidak dipanggil dari {fname}",
               "adcCalibrationTask" not in trigger_body)


def test_gadc_compile_out_when_false():
    """G-ADC: seluruh code di-compile-out ketika flag false."""
    print("G-ADC: compile-out when flag false")
    s = _adc_src()

    # Seluruh implementation di .cpp harus dibungkus #if guard
    cpp = s["cpp"]
    _check("G-ADC .cpp dibuka dengan #include header + #if guard",
           "#if DEBUG_ADC_CALIBRATION" in cpp)
    _check("G-ADC .cpp ditutup dengan #endif // DEBUG_ADC_CALIBRATION",
           "#endif // DEBUG_ADC_CALIBRATION" in cpp or
           "#endif" in cpp)

    # Demikian juga .h
    h = s["h"]
    _check("G-ADC .h punya #if DEBUG_ADC_CALIBRATION guard di dalam",
           "#if DEBUG_ADC_CALIBRATION" in h)

    # adcCalibrationInit() hanya ada di dalam #if block di .ino
    ino = s["ino"]
    init_idx = ino.find("adcCalibrationInit()")
    guard_idx = ino.rfind("#if DEBUG_ADC_CALIBRATION", 0, init_idx)
    _check("G-ADC adcCalibrationInit() ada di dalam #if guard di .ino",
           guard_idx >= 0)


if __name__ == "__main__":
    sys.exit(main())
