"""Minimal PE32 image loader for the recompiler: sections, relocations, imports."""
import pefile

class PE:
    def __init__(self, path):
        pe = pefile.PE(path)
        self.pe = pe
        self.base = pe.OPTIONAL_HEADER.ImageBase
        self.size = pe.OPTIONAL_HEADER.SizeOfImage
        self.entry = self.base + pe.OPTIONAL_HEADER.AddressOfEntryPoint
        self.image = bytearray(pe.get_memory_mapped_image(ImageBase=self.base))
        self.image += bytes(self.size - len(self.image))
        self.sections = []
        for s in pe.sections:
            name = s.Name.rstrip(b'\0').decode()
            self.sections.append((name, self.base + s.VirtualAddress, max(s.Misc_VirtualSize, s.SizeOfRawData), s.Characteristics))
        self.relocs = []          # addresses holding absolute 32-bit pointers
        for blk in getattr(pe, 'DIRECTORY_ENTRY_BASERELOC', []):
            for e in blk.entries:
                if e.type == 3: self.relocs.append(self.base + e.rva)
        self.imports = []         # (iat_addr, dll, name)
        for d in pe.DIRECTORY_ENTRY_IMPORT:
            for i in d.imports:
                nm = i.name.decode() if i.name else '#%d' % i.ordinal
                self.imports.append((i.address, d.dll.decode().upper(), nm))
        self.stack_reserve = pe.OPTIONAL_HEADER.SizeOfStackReserve

    def section(self, name):
        for s in self.sections:
            if s[0] == name: return s
    def u32(self, a):
        o = a - self.base
        return int.from_bytes(self.image[o:o + 4], 'little')
    def bytes_at(self, a, n):
        o = a - self.base
        return bytes(self.image[o:o + n])
