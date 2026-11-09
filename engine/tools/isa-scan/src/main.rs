// SPDX-License-Identifier: GPL-2.0-only
// Adapted from SpartanPort's iced-x86 scanner:
// https://github.com/FelipePrado0/SpartanPort/blob/main/launcher-rust/src/runner/isa_rewrite.rs
// Modified for Lantern Engine on 2026-10-31: read BBPROBE5, emit MOVBE/BMI1/LZCNT only,
// and skip scanning entirely when the host supports all three sets.


use std::collections::HashSet;
use std::env;
use std::fs;
use std::path::{Path, PathBuf};

use iced_x86::{Code, Decoder, DecoderOptions, FlowControl, Instruction, Mnemonic, OpKind, Register};
use sha2::{Digest, Sha256};

const MAGIC: &[u8; 8] = b"G3ISA1\0\0";
const NO_REG: u8 = 0xff;
const HEADER_SIZE: usize = 64;
const RECORD_SIZE: usize = 64;
const MAX_INSTRUCTION: usize = 15;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Kind {
    MovbeLoad,
    MovbeStore,
    Lzcnt,
    Tzcnt,
    Andn,
    Bextr,
    Blsi,
    Blsmsk,
    Blsr,
}

impl Kind {
    fn id(self) -> u8 {
        match self {
            Self::MovbeLoad => 1,
            Self::MovbeStore => 2,
            Self::Lzcnt => 3,
            Self::Tzcnt => 4,
            Self::Andn => 5,
            Self::Bextr => 6,
            Self::Blsi => 7,
            Self::Blsmsk => 8,
            Self::Blsr => 9,
        }
    }

    fn set(self) -> &'static str {
        match self {
            Self::MovbeLoad | Self::MovbeStore => "MOVBE",
            Self::Lzcnt => "LZCNT",
            Self::Tzcnt | Self::Andn | Self::Bextr | Self::Blsi | Self::Blsmsk | Self::Blsr => "BMI1",
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Mode {
    Jmp = 0,
    JmpRelocate = 1,
    Int3 = 2,
    Unsupported = 3,
}

#[derive(Clone, Debug)]
struct Site {
    offset: u64,
    length: u8,
    kind: Kind,
    width: u8,
    mode: Mode,
    dst: u8,
    src1: u8,
    src2: u8,
    mem_slot: u8,
    base: u8,
    index: u8,
    scale: u8,
    mem_flags: u8,
    disp: i64,
    original: [u8; MAX_INSTRUCTION],
    relocated: Vec<u8>,
}

#[derive(Clone, Copy, Debug)]
struct Segment {
    address: u64,
    size: u64,
    flags: u64,
}

fn take_u64(data: &[u8], at: &mut usize) -> Result<u64, String> {
    let end = at.checked_add(8).ok_or("linked image offset overflow")?;
    let bytes = data.get(*at..end).ok_or("truncated linked image metadata")?;
    *at = end;
    Ok(u64::from_le_bytes(bytes.try_into().map_err(|_| "invalid uint64")?))
}

fn checked_skip(data: &[u8], at: &mut usize, count: u64, width: u64) -> Result<(), String> {
    let bytes = count.checked_mul(width).ok_or("linked image metadata size overflow")?;
    let bytes = usize::try_from(bytes).map_err(|_| "linked image metadata too large")?;
    let end = at.checked_add(bytes).ok_or("linked image offset overflow")?;
    if end > data.len() {
        return Err("truncated linked image metadata".into());
    }
    *at = end;
    Ok(())
}

// Lantern's BBPROBE5 and SpartanPort's G3PROBE5 have the same linked-image
// field layout. The executable segment table precedes import names and relocs.
fn read_boot_linked(data: &[u8]) -> Result<(Vec<Segment>, Vec<u8>), String> {
    if data.len() < 8 || (&data[..8] != b"BBPROBE5" && &data[..8] != b"G3PROBE5") {
        return Err("not a BBPROBE5 linked image".into());
    }
    let mut at = 8usize;
    let image_size = take_u64(data, &mut at)?;
    let _entry = take_u64(data, &mut at)?;
    let segment_count = take_u64(data, &mut at)?;
    let reloc_count = take_u64(data, &mut at)?;
    let import_count = take_u64(data, &mut at)?;
    let _capabilities = take_u64(data, &mut at)?;

    if image_size == 0 || image_size > 512 * 1024 * 1024 ||
       segment_count == 0 || segment_count > 64 ||
       reloc_count > 1_000_000 || import_count > 100_000 {
        return Err("linked image limits exceeded".into());
    }

    let _procparam = take_u64(data, &mut at)?;
    for _ in 0..4 { let _ = take_u64(data, &mut at)?; }
    let module_count = take_u64(data, &mut at)?;
    if module_count == 0 || module_count > 16 {
        return Err("invalid linked module count".into());
    }
    checked_skip(data, &mut at, module_count, 7 * 8)?;

    let binding_count = take_u64(data, &mut at)?;
    if binding_count > import_count {
        return Err("invalid native binding count".into());
    }
    checked_skip(data, &mut at, binding_count, 3 * 8)?;

    let mut segments = Vec::with_capacity(segment_count as usize);
    for _ in 0..segment_count {
        let address = take_u64(data, &mut at)?;
        let size = take_u64(data, &mut at)?;
        let flags = take_u64(data, &mut at)?;
        if address > image_size || size > image_size - address {
            return Err("segment lies outside linked image".into());
        }
        segments.push(Segment { address, size, flags });
    }

    checked_skip(data, &mut at, import_count, 128)?;
    checked_skip(data, &mut at, reloc_count, 4 * 8)?;

    let image_len = usize::try_from(image_size).map_err(|_| "linked image too large")?;
    let end = at.checked_add(image_len).ok_or("linked image size overflow")?;
    if end != data.len() {
        return Err("linked image is truncated or has trailing data".into());
    }
    Ok((segments, data[at..end].to_vec()))
}

fn kind_of(ins: &Instruction) -> Option<Kind> {
    Some(match ins.mnemonic() {
        Mnemonic::Movbe if ins.op0_kind() == OpKind::Register => Kind::MovbeLoad,
        Mnemonic::Movbe => Kind::MovbeStore,
        Mnemonic::Lzcnt => Kind::Lzcnt,
        Mnemonic::Tzcnt => Kind::Tzcnt,
        Mnemonic::Andn => Kind::Andn,
        Mnemonic::Bextr => Kind::Bextr,
        Mnemonic::Blsi => Kind::Blsi,
        Mnemonic::Blsmsk => Kind::Blsmsk,
        Mnemonic::Blsr => Kind::Blsr,
        _ => return None,
    })
}

fn register_number(register: Register) -> u8 {
    if register == Register::None { NO_REG } else { register.number() as u8 }
}

fn site_of(ins: &Instruction, kind: Kind, bytes: &[u8]) -> Site {
    let mut registers = [NO_REG; 3];
    let mut mem_slot = 0;
    let mut width = 0;
    for operand in 0..ins.op_count() {
        match ins.op_kind(operand) {
            OpKind::Register => {
                let register = ins.op_register(operand);
                if let Some(slot) = registers.get_mut(operand as usize) {
                    *slot = register_number(register);
                }
                if width == 0 { width = register.size() * 8; }
            }
            OpKind::Memory => mem_slot = if operand == 0 { 1 } else { 2 },
            _ => {}
        }
    }

    let [dst, src1, src2] = registers;
    let mut original = [0u8; MAX_INSTRUCTION];
    original[..bytes.len()].copy_from_slice(bytes);
    let mut site = Site {
        offset: ins.ip(),
        length: ins.len() as u8,
        kind,
        width: width as u8,
        mode: Mode::Jmp,
        dst, src1, src2, mem_slot,
        base: NO_REG,
        index: NO_REG,
        scale: 0,
        mem_flags: 0,
        disp: 0,
        original,
        relocated: Vec::new(),
    };

    let mut unsupported = ins.segment_prefix() != Register::None || ins.has_lock_prefix();
    if mem_slot != 0 {
        unsupported |= [ins.memory_base(), ins.memory_index()]
            .iter()
            .any(|&register| register != Register::None && register.size() != 8);
        site.scale = ins.memory_index_scale() as u8;
        if ins.is_ip_rel_memory_operand() {
            site.mem_flags = 1;
            site.disp = ins.ip_rel_memory_address() as i64;
        } else {
            site.base = register_number(ins.memory_base());
            site.index = register_number(ins.memory_index());
            site.disp = ins.memory_displacement64() as i64;
        }
    }

    site.mode = if unsupported {
        Mode::Unsupported
    } else if ins.len() >= 5 {
        Mode::Jmp
    } else {
        Mode::Int3
    };
    site
}

fn is_plain(ins: &Instruction, end: u64) -> bool {
    ins.code() != Code::INVALID &&
    ins.flow_control() == FlowControl::Next &&
    !ins.is_ip_rel_memory_operand() &&
    !ins.has_rep_prefix() &&
    !ins.has_repne_prefix() &&
    kind_of(ins).is_none() &&
    ins.next_ip() <= end
}

fn scan_segment(code: &[u8], address: u64) -> Vec<Site> {
    let Ok(mut decoder) = Decoder::try_with_ip(64, code, address, DecoderOptions::NONE) else {
        return Vec::new();
    };
    let end = address.saturating_add(code.len() as u64);
    let mut sites: Vec<Site> = Vec::new();
    let mut targets = HashSet::new();
    let mut candidates = Vec::new();
    let mut pending = None;

    while decoder.can_decode() {
        let start = decoder.position();
        let ins = decoder.decode();
        let bytes = code.get(start..decoder.position()).unwrap_or_default();

        if matches!(ins.flow_control(),
            FlowControl::UnconditionalBranch | FlowControl::ConditionalBranch | FlowControl::Call)
            && ins.op0_kind() == OpKind::NearBranch64 {
            targets.insert(ins.near_branch_target());
        }

        if let Some(site) = pending.take() {
            if is_plain(&ins, end) {
                candidates.push((site, ins.ip(), bytes.to_vec()));
            }
        }

        if let Some(kind) = kind_of(&ins) {
            let site = site_of(&ins, kind, bytes);
            if site.mode == Mode::Int3 && ins.len() == 4 {
                pending = Some(sites.len());
            }
            sites.push(site);
        }
    }

    for (site, ip, relocated) in candidates {
        if let Some(site) = sites.get_mut(site).filter(|_| !targets.contains(&ip)) {
            site.mode = Mode::JmpRelocate;
            site.relocated = relocated;
        }
    }
    sites
}

fn padded(bytes: &[u8]) -> [u8; MAX_INSTRUCTION] {
    let mut out = [0u8; MAX_INSTRUCTION];
    out[..bytes.len()].copy_from_slice(bytes);
    out
}

fn encode_table(image: &[u8], sites: &[Site]) -> Result<Vec<u8>, String> {
    if sites.len() > (1 << 20) {
        return Err("too many instruction sites".into());
    }
    let mut table = Vec::with_capacity(HEADER_SIZE + RECORD_SIZE * sites.len());
    table.extend_from_slice(MAGIC);
    table.extend_from_slice(&(image.len() as u64).to_le_bytes());
    table.extend_from_slice(&Sha256::digest(image));
    table.extend_from_slice(&(sites.len() as u32).to_le_bytes());
    let unsupported = sites.iter().filter(|s| s.mode == Mode::Unsupported).count();
    table.extend_from_slice(&(unsupported as u32).to_le_bytes());
    table.extend_from_slice(&[0; 8]);

    let mut sorted: Vec<&Site> = sites.iter().collect();
    sorted.sort_by_key(|s| s.offset);
    for site in sorted {
        if site.relocated.len() > MAX_INSTRUCTION {
            return Err("relocated instruction exceeds x86 instruction length".into());
        }
        table.extend_from_slice(&site.offset.to_le_bytes());
        table.extend_from_slice(&[
            site.length, site.kind.id(), site.width, site.mode as u8,
            site.dst, site.src1, site.src2, site.mem_slot,
            site.base, site.index, site.scale, site.mem_flags,
            0, 0, 0, 0,
        ]);
        table.extend_from_slice(&site.disp.to_le_bytes());
        table.extend_from_slice(&site.original);
        table.push(site.relocated.len() as u8);
        table.extend_from_slice(&padded(&site.relocated));
        table.push(0);
    }
    Ok(table)
}

#[cfg(target_arch = "x86_64")]
#[allow(unused_unsafe)]
fn missing_cpu_sets() -> Vec<&'static str> {
    use std::arch::x86_64::{__cpuid, __cpuid_count};
    // Use CPUID directly so this check is available in both GNU and MSVC Windows builds.
    let (leaf1_ecx, leaf7_ebx, extended_ecx) = unsafe {
        let basic_max = __cpuid(0).eax;
        let leaf1_ecx = if basic_max >= 1 { __cpuid(1).ecx } else { 0 };
        let leaf7_ebx = if basic_max >= 7 { __cpuid_count(7, 0).ebx } else { 0 };
        let extended_max = __cpuid(0x8000_0000).eax;
        let extended_ecx = if extended_max >= 0x8000_0001 {
            __cpuid(0x8000_0001).ecx
        } else {
            0
        };
        (leaf1_ecx, leaf7_ebx, extended_ecx)
    };
    let mut missing = Vec::new();
    if leaf1_ecx & (1 << 22) == 0 { missing.push("MOVBE"); }
    if leaf7_ebx & (1 << 3) == 0 { missing.push("BMI1"); }
    if extended_ecx & (1 << 5) == 0 { missing.push("LZCNT"); }
    missing
}

#[cfg(not(target_arch = "x86_64"))]
fn missing_cpu_sets() -> Vec<&'static str> {
    vec!["x86-64 host required"]
}

fn scan(boot: &Path, out: &Path) -> Result<String, String> {
    let missing = if env::var("BB_ISA_FORCE").as_deref() == Ok("1") {
        vec!["MOVBE", "BMI1", "LZCNT"]
    } else {
        missing_cpu_sets()
    };
    if missing.is_empty() {
        return Ok("CPU: native MOVBE, BMI1 and LZCNT; scanner skipped, original code will execute natively".into());
    }
    let data = fs::read(boot).map_err(|e| format!("{}: {e}", boot.display()))?;
    let (segments, image) = read_boot_linked(&data)?;
    let mut sites = Vec::new();

    for segment in segments.iter().filter(|s| s.flags & 1 != 0) {
        let start = usize::try_from(segment.address).map_err(|_| "segment offset too large")?;
        let size = usize::try_from(segment.size).map_err(|_| "segment size too large")?;
        let end = start.checked_add(size).ok_or("segment range overflow")?;
        let code = image.get(start..end).ok_or("executable segment outside linked image")?;
        sites.extend(scan_segment(code, segment.address));
    }

    let table = encode_table(&image, &sites)?;
    fs::create_dir_all(out).map_err(|e| format!("{}: {e}", out.display()))?;
    let target = out.join("isa-rewrite.bin");
    let temporary = out.join("isa-rewrite.bin.tmp");
    fs::write(&temporary, table).map_err(|e| format!("{}: {e}", temporary.display()))?;
    if target.exists() {
        fs::remove_file(&target).map_err(|e| format!("{}: {e}", target.display()))?;
    }
    fs::rename(&temporary, &target).map_err(|e| format!("{}: {e}", target.display()))?;

    let count_set = |set: &str| sites.iter().filter(|s| s.kind.set() == set).count();
    let relocated = sites.iter().filter(|s| s.mode == Mode::JmpRelocate).count();
    let int3 = sites.iter().filter(|s| s.mode == Mode::Int3).count();
    let unsupported = sites.iter().filter(|s| s.mode == Mode::Unsupported).count();
    Ok(format!(
        "CPU missing {}; ISA: {} sites (MOVBE {}, BMI1 {}, LZCNT {}); {} relocated, {} int3, {} unsupported",
        missing.join(", "), sites.len(), count_set("MOVBE"), count_set("BMI1"), count_set("LZCNT"),
        relocated, int3, unsupported
    ))
}

fn main() {
    let args: Vec<String> = env::args().collect();
    if args.len() != 3 {
        eprintln!("Usage: bb-isa-scan <boot-linked.bin> <output-directory>");
        std::process::exit(2);
    }
    match scan(&PathBuf::from(&args[1]), &PathBuf::from(&args[2])) {
        Ok(summary) => println!("{summary}"),
        Err(error) => {
            eprintln!("ISA: rewrite table unavailable ({error})");
            std::process::exit(1);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn bytes(hex: &str) -> Vec<u8> {
        hex.as_bytes().chunks_exact(2)
            .map(|pair| u8::from_str_radix(std::str::from_utf8(pair).unwrap(), 16).unwrap())
            .collect()
    }

    fn lantern_boot_linked(code: &[u8]) -> Vec<u8> {
        let size = code.len() as u64;
        let words = [
            size, 0, 1, 0, 0, 1,             // BBPROBE5 header
            0,                               // procparam
            0, 0, 0, 0,                      // main TLS
            1,                               // one linked module
            0, size, 0, 0, 0, 0, 0,          // module metadata
            0,                               // no native bindings
            0, size, 5,                      // one executable segment
        ];
        let mut data = b"BBPROBE5".to_vec();
        for word in words {
            data.extend_from_slice(&word.to_le_bytes());
        }
        data.extend_from_slice(code);
        data
    }

    #[test]
    fn reads_lantern_bbprobe5_and_scans_only_the_requested_instruction_sets() {
        let code = bytes("0f38f108c4e270f3c9f30fbdc1c3");
        let boot = lantern_boot_linked(&code);
        let (segments, image) = read_boot_linked(&boot).unwrap();
        assert_eq!(segments.len(), 1);
        assert_eq!(segments[0].address, 0);
        assert_eq!(segments[0].size, code.len() as u64);
        assert_eq!(segments[0].flags & 1, 1);
        assert_eq!(image, code);

        let sites = scan_segment(&image, 0);
        assert_eq!(sites.len(), 3);
        assert_eq!(sites[0].kind, Kind::MovbeStore);
        assert_eq!(sites[1].kind, Kind::Blsr);
        assert_eq!(sites[2].kind, Kind::Lzcnt);
    }

    #[test]
    fn scans_movbe_bmi1_and_lzcnt_without_f16c() {
        let code = bytes("0f38f108c4e270f3c9f30fbdc1c3");
        let sites = scan_segment(&code, 0x1000);
        assert_eq!(sites.len(), 3);
        assert_eq!(sites[0].kind, Kind::MovbeStore);
        assert_eq!(sites[1].kind, Kind::Blsr);
        assert_eq!(sites[2].kind, Kind::Lzcnt);
        assert!(sites.iter().all(|s| s.mode != Mode::Unsupported));
    }

    #[test]
    fn writes_the_loader_record_layout() {
        let image = bytes("c4e270f3c9");
        let site = scan_segment(&image, 0).remove(0);
        let table = encode_table(&image, &[site]).unwrap();
        assert_eq!(&table[..8], MAGIC);
        assert_eq!(&table[8..16], (image.len() as u64).to_le_bytes());
        assert_eq!(u32::from_le_bytes(table[48..52].try_into().unwrap()), 1);
        assert_eq!(table.len(), HEADER_SIZE + RECORD_SIZE);
    }
}
