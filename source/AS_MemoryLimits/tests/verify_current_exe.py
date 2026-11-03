"""Read-only verification of the audited PE against MemoryLimits.hpp (stdlib only).

Does not load or execute the PE, compile the plugin, or write to the input file.
This verifies static build constants, not execution of the native plugin.
"""
import argparse
import hashlib
import json
import re
import struct
from pathlib import Path


def require(condition, message):
    if not condition:
        raise ValueError(message)


def verify(exe):
    header = (Path(__file__).resolve().parents[1] / "MemoryLimits.hpp").read_text(encoding="utf8")
    raw = exe.read_bytes()

    def constant(name):
        match = re.search(r"\b" + name + r" = (0x[0-9a-f]+|[0-9]+);", header)
        require(match is not None, "Missing constant: " + name)
        return int(match[1], 0)

    expected_hash = re.search(r'server_sha256\[\] = "([0-9a-f]{64})";', header)[1]
    require(len(raw) == constant("file_size"), "Unsupported EXE size")
    digest = hashlib.sha256(raw).hexdigest()
    require(digest == expected_hash, "Unsupported EXE SHA-256")
    require(raw[:2] == b"MZ", "Not an MZ file")
    pe = struct.unpack_from("<I", raw, 0x3c)[0]
    require(raw[pe:pe+4] == b"PE\0\0", "Not a PE file")
    machine, count, stamp = struct.unpack_from("<HHI", raw, pe + 4)
    optional = pe + 24
    require(machine == 0x8664 and struct.unpack_from("<H", raw, optional)[0] == 0x20b, "Not x64 PE32+")
    require(stamp == constant("timestamp"), "Unexpected PE timestamp")
    image_size = struct.unpack_from("<I", raw, optional + 56)[0]
    require(image_size == constant("image_size"), "Unexpected image size")
    base = struct.unpack_from("<Q", raw, optional + 24)[0]
    section_offset = optional + struct.unpack_from("<H", raw, pe + 20)[0]
    sections = []
    for i in range(count):
        at = section_offset + 40 * i
        virtual_size, rva, size, offset = struct.unpack_from("<IIII", raw, at + 8)
        flags = struct.unpack_from("<I", raw, at + 36)[0]
        require(offset + size <= len(raw), "Section exceeds file")
        require(rva + max(virtual_size, size) <= image_size, "Section exceeds image")
        sections.append((rva, size, offset, flags))

    def file_offset(rva, size):
        for start, length, offset, _ in sections:
            if start <= rva and rva + size <= start + length:
                return offset + rva - start
        raise ValueError("RVA has no complete file mapping: " + hex(rva))

    def read(rva, size):
        offset = file_offset(rva, size)
        return raw[offset:offset+size]

    def qword(rva):
        return struct.unpack("<Q", read(rva, 8))[0]

    def executable(rva):
        return any(start <= rva < start + size and flags & 0x20000000 for start,size,_,flags in sections)

    pattern = r'\{L"(\w+)", (\d+), (\d+), (0x[0-9a-f]+), \{([^}]+)\}\}'
    sites = re.findall(pattern, header)
    require(len(sites) == 6, "Expected six sites")
    report = []
    for key, vanilla, default, rva_text, byte_text in sites:
        rva = int(rva_text, 16)
        original = bytes(int(part,0) for part in byte_text.split(","))
        require(len(original) == 24 and read(rva,24) == original, key + ": original bytes mismatch")
        require(executable(rva), key + ": site is not executable")
        require(original[:4] == bytes.fromhex("c7442420"), key + ": wrong opcode")
        require(struct.unpack_from("<I", original, 4)[0] == int(vanilla)*1024*1024, key + ": wrong vanilla limit")
        require(int(vanilla) <= int(default) <= 1024, key + ": invalid default")
        entry = {"key":key,"rva":hex(rva),"immediate_file_offset":hex(file_offset(rva+4,4)),"vanilla_mb":int(vanilla),"default_mb":int(default)}
        if key in {"GameMB","WorldMB","MissionMB","GameLibMB"}:
            slot = rva + 22 + struct.unpack_from("<i",original,18)[0]
            name_rva = qword(slot) - base
            # Read only within its mapped section, until the first NUL.
            value = read(name_rva,80).split(b"\0")[0].decode("ascii")
            expected = key.removesuffix("MB") + ".sproj"
            require(value == "scripts/editor/projects/" + expected, key + ": wrong module")
            entry["module_project"] = value
        report.append(entry)

    table = constant("engine_vtable_rva")
    rtti = constant("engine_rtti_rva")
    require(qword(table-8) == base+rtti, "RTTI pointer mismatch")
    locator = struct.unpack("<6I",read(rtti,24))
    require(locator[0] == 1 and locator[5] == rtti, "Invalid x64 RTTI locator")
    require(read(locator[3]+16,23).split(b"\0")[0] == b".?AVIStaticBuffer@enf@@", "Unexpected Engine type")
    methods_text = re.search(r"engine_vtable_rvas\{([^}]+)\}",header,re.S)[1]
    methods = [int(v.strip(),0) for v in methods_text.split(",")]
    require(len(methods) == 11, "Expected 11 virtual methods")
    for i,rva in enumerate(methods):
        require(qword(table+i*8) == base+rva and executable(rva), "Virtual slot mismatch: " + str(i))
    require(methods[7] == constant("engine_allocator_rva"), "Allocator slot mismatch")
    context = constant("engine_context_rva")
    # Constructor's observed store must resolve to the configured global pointer.
    instruction = read(0x394f27,7)
    require(instruction[:3] == bytes.fromhex("4c8935"), "Engine context store changed")
    require(0x394f27+7+struct.unpack_from("<i",instruction,3)[0] == context, "Wrong Engine global pointer")
    # Static checks of the actual constructor/allocator field accesses used by EngineSpill.
    for rva, expected in {
        0x395231:"49899698010000", 0x3a2a55:"418d5068", 0x3a2a8e:"488d4b40",
        0x3a2a9d:"44887b2c", 0x3a2ad4:"488b43204889431044897328",
        0x3a2ae3:"48894318", 0x3a2965:"38412c", 0x3a2972:"488d7940",
        0x3a2988:"488b5e10", 0x3a298c:"4883c3074883e3f8", 0x3a2998:"48894610"
    }.items():
        require(read(rva,len(bytes.fromhex(expected))) == bytes.fromhex(expected), "Engine layout opcode mismatch at " + hex(rva))
    return {"verified":True,"scope":"static PE and source constants only","sha256":digest,"file_size":len(raw),
            "image_size":hex(image_size),"timestamp":hex(stamp),"sites":report,
            "engine_context_rva":hex(context),"engine_vtable_rva":hex(table),"engine_rtti_rva":hex(rtti),
            "engine_methods":[hex(v) for v in methods],"native_compilation":"not performed","server_execution":"not performed"}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("exe", type=Path)
    args = parser.parse_args()
    print(json.dumps(verify(args.exe),indent=2))
