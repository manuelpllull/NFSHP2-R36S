//! nfshp2-unpack: the installer's disc work, in parallel.
//!
//!     nfshp2-unpack <disc.iso> <dest dir>   < jobs
//!
//! nfshp2_install.py plans the install and sends one job per line on stdin,
//! fields separated by tabs, offsets and sizes in bytes from the image's
//! start:
//!
//!     copy    OFF SIZE REL                         a file as it is
//!     expand  OFF SIZE OUT_SIZE REL                a COMP track bundle
//!     bank    MSH_OFF MSH_SIZE MSB_OFF MSB_SIZE NAME REL
//!                                                  a .MSH/.MSB pair -> .SBK
//!
//! One thread does the copies in order (they are bound by the card), and
//! one per core does the bundles and banks, largest first. Each finished
//! job prints `done<TAB>BYTES<TAB>REL`; the first failure prints
//! `error<TAB>message` and exits 2. Every file is written to REL.part and
//! renamed, as the installer's write_atomic does.
//!
//! The bundle and bank code follows tools/lzdecompress.py and
//! tools/audio_pack.py's `build_bank` byte for byte (docs/formats/
//! sound-bank.md has the .SBK layout).

use std::fs::{self, File};
use std::io::{self, BufRead, Write};
use std::os::unix::fs::FileExt;
use std::path::{Path, PathBuf};
use std::process;
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{mpsc, Arc};
use std::thread;

type Res<T> = Result<T, String>;

enum Kind {
    Copy,
    Expand { out_size: usize },
    Bank { msb_off: u64, msb_size: usize, name: Vec<u8> },
}

struct Job {
    kind: Kind,
    off: u64,
    size: usize,
    rel: String,
}

fn num<T: std::str::FromStr>(s: &str) -> Res<T> {
    s.parse().map_err(|_| format!("bad number in a job: {s:?}"))
}

fn parse(line: &str) -> Res<Job> {
    let f: Vec<&str> = line.split('\t').collect();
    let job = match (f[0], f.len()) {
        ("copy", 4) => Job { kind: Kind::Copy, off: num(f[1])?, size: num(f[2])?, rel: f[3].into() },
        ("expand", 5) => Job {
            kind: Kind::Expand { out_size: num(f[3])? },
            off: num(f[1])?,
            size: num(f[2])?,
            rel: f[4].into(),
        },
        ("bank", 7) => Job {
            kind: Kind::Bank { msb_off: num(f[3])?, msb_size: num(f[4])?, name: f[5].as_bytes().to_vec() },
            off: num(f[1])?,
            size: num(f[2])?,
            rel: f[6].into(),
        },
        _ => return Err(format!("unknown job: {line:?}")),
    };
    Ok(job)
}

fn read_at(iso: &File, off: u64, size: usize) -> Res<Vec<u8>> {
    let mut buf = vec![0u8; size];
    iso.read_exact_at(&mut buf, off).map_err(|e| format!("reading the disc image: {e}"))?;
    Ok(buf)
}

fn part_of(dest: &Path) -> PathBuf {
    let mut p = dest.as_os_str().to_owned();
    p.push(".part");
    PathBuf::from(p)
}

fn open_part(dest: &Path) -> Res<(File, PathBuf)> {
    if let Some(dir) = dest.parent() {
        fs::create_dir_all(dir).map_err(|e| format!("{}: {e}", dir.display()))?;
    }
    let tmp = part_of(dest);
    let f = File::create(&tmp).map_err(|e| format!("{}: {e}", tmp.display()))?;
    Ok((f, tmp))
}

fn finish(f: File, tmp: &Path, dest: &Path) -> Res<()> {
    drop(f);
    fs::rename(tmp, dest).map_err(|e| format!("{}: {e}", dest.display()))
}

fn write_atomic(dest: &Path, data: &[u8]) -> Res<()> {
    let (mut f, tmp) = open_part(dest)?;
    f.write_all(data).map_err(|e| format!("{}: {e}", tmp.display()))?;
    finish(f, &tmp, dest)
}

fn copy(iso: &File, off: u64, size: usize, dest: &Path) -> Res<()> {
    const CHUNK: usize = 4 << 20;
    let (mut f, tmp) = open_part(dest)?;
    let mut buf = vec![0u8; CHUNK.min(size.max(1))];
    let mut done = 0usize;
    while done < size {
        let n = CHUNK.min(size - done);
        iso.read_exact_at(&mut buf[..n], off + done as u64)
            .map_err(|e| format!("reading the disc image: {e}"))?;
        f.write_all(&buf[..n]).map_err(|e| format!("{}: {e}", tmp.display()))?;
        done += n;
    }
    finish(f, &tmp, dest)
}

// ------------------------------------------------------------ track bundles

fn u16le(b: &[u8], o: usize) -> u16 {
    u16::from_le_bytes([b[o], b[o + 1]])
}
fn u32le(b: &[u8], o: usize) -> u32 {
    u32::from_le_bytes([b[o], b[o + 1], b[o + 2], b[o + 3]])
}

/// The installer's `expand`: a COMP container's LZ, 16 items per control
/// word, a 12-bit distance and a 4-bit length (3..18).
fn expand(buf: &[u8], want: usize) -> Res<Vec<u8>> {
    const BAD: &str = "a track bundle is damaged";
    if buf.len() < 16 || &buf[..4] != b"COMP" {
        return Err("a track bundle isn't a COMP container".into());
    }
    let method = u16le(buf, 6);
    let out_size = u32le(buf, 8) as usize;
    let end = u32le(buf, 12) as usize;
    if end < 16 || end > buf.len() {
        return Err(BAD.into());
    }
    if method == 1 {
        return Ok(buf[16..end].to_vec());
    }
    let at = |i: usize| buf.get(i).copied().ok_or_else(|| BAD.to_string());
    let mut out: Vec<u8> = Vec::with_capacity(out_size);
    let (mut src, mut ctrl) = (16usize, 1u32);
    while src != end {
        if src > end {
            return Err(BAD.into());
        }
        if ctrl == 1 {
            ctrl = at(src)? as u32 | 0x10000 | (at(src + 1)? as u32) << 8;
            src += 2;
        }
        let n = if src + 0x20 <= end { 16 } else { 1 };
        for _ in 0..n {
            if ctrl & 1 != 0 {
                let b0 = at(src)? as usize;
                let dist = ((b0 & 0xF0) << 4) | at(src + 1)? as usize;
                let length = 3 + (b0 & 0x0F);
                src += 2;
                if dist == 0 || dist > out.len() {
                    return Err(BAD.into());
                }
                let p = out.len() - dist;
                if dist >= length {
                    out.extend_from_within(p..p + length);
                } else {
                    for k in 0..length {
                        let v = out[p + k];
                        out.push(v);
                    }
                }
            } else {
                out.push(at(src)?);
                src += 1;
            }
            ctrl >>= 1;
        }
    }
    if out.len() != out_size {
        return Err(format!("a track bundle expanded to {} bytes, not {}", out.len(), out_size));
    }
    if out_size != want {
        return Err(format!("a track bundle says {out_size} bytes, the plan {want}"));
    }
    Ok(out)
}

// ------------------------------------------------------------- sound banks

const MSH_HEADER: usize = 12;
const MSH_PATCH: usize = 52;
const SPU_FRAME: usize = 16;
const SPU_SAMPLES: usize = 28;
const LOOP_FLAG: u8 = 0x01;
const RIGHT_FLAG: u8 = 0x10;
const LEAD_IN: usize = 28;
const SBK_MAGIC: u32 = 0x314B_4253; // 'SBK1'
const SBK_HEADER: usize = 32;
const SBK_PATCH: usize = 128;
const COEF: [(i32, i32); 5] = [(0, 0), (60, 0), (115, -52), (98, -55), (122, -60)];

fn dsp_coefs() -> [i16; 16] {
    let mut c = [0i16; 16];
    for (i, &(a, b)) in COEF.iter().enumerate() {
        c[i * 2] = (a * 32) as i16;
        c[i * 2 + 1] = (b * 32) as i16;
    }
    c
}

fn bhash(s: &[u8]) -> u32 {
    s.iter().fold(0xFFFF_FFFFu32, |h, &c| h.wrapping_mul(33).wrapping_add(c as u32))
}

struct Patch<'a> {
    name: &'a [u8],
    pan: i8,
    loop_channel: u8,
    pitch: u16,
    volume: u16,
    effect: u16,
    adsr: u32,
    data: &'a [u8],
}

fn read_bank<'a>(head: &'a [u8], body: &'a [u8], bank: &str) -> Res<Vec<Patch<'a>>> {
    let count = if head.len() >= MSH_HEADER { u32le(head, 8) as usize } else { 0 };
    if !(MSH_HEADER + count * MSH_PATCH <= head.len() && head.len() <= 0x10000) {
        return Err(format!("{bank}.MSH isn't a sound bank's index ({} bytes)", head.len()));
    }
    let mut offs = Vec::with_capacity(count);
    let mut out = Vec::with_capacity(count);
    for i in 0..count {
        let r = MSH_HEADER + i * MSH_PATCH;
        let raw = &head[r..r + 32];
        let name = &raw[..raw.iter().position(|&b| b == 0).unwrap_or(32)];
        offs.push(u32le(head, r + 40) as usize);
        out.push(Patch {
            name,
            pan: head[r + 32] as i8,
            loop_channel: head[r + 33],
            pitch: u16le(head, r + 34),
            volume: u16le(head, r + 36),
            effect: u16le(head, r + 38),
            adsr: u32le(head, r + 44),
            data: &[],
        });
    }
    for i in 0..count {
        let end = if i + 1 < count { offs[i + 1] } else { body.len() };
        if !(offs[i] <= end && end <= body.len()) {
            return Err(format!("{bank}: patch {i} runs outside the .MSB"));
        }
        out[i].data = &body[offs[i]..end];
    }
    Ok(out)
}

/// audio_convert.decode_adpcm, for what build_bank uses of it: the sample
/// count and the lead-in's last two samples.
fn decode_adpcm(data: &[u8]) -> Vec<i16> {
    let mut out = Vec::new();
    let (mut h1, mut h2) = (0i32, 0i32);
    let mut i = 0;
    while data.len() >= SPU_FRAME && i + SPU_FRAME <= data.len() {
        let head = data[i];
        let shift = (head & 0x0F) as u32;
        let pred = (head >> 4) as usize;
        let (f0, f1) = COEF[if pred > 4 { 0 } else { pred }];
        let flag = data[i + 1];
        if flag == 7 {
            break;
        }
        for j in 0..14 {
            let byte = data[i + 2 + j];
            for nib in [byte & 0x0F, byte >> 4] {
                let mut v = (nib as i32) << 12;
                if v & 0x8000 != 0 {
                    v -= 0x10000;
                }
                let s = ((v >> shift) + ((f0 * h1 + f1 * h2 + 32) >> 6)).clamp(-32768, 32767);
                h2 = h1;
                h1 = s;
                out.push(s as i16);
            }
        }
        if flag == 1 {
            break;
        }
        i += SPU_FRAME;
    }
    out
}

/// audio_pack.transcode: SPU frames -> DSPADPCM frames, nothing decoded.
fn transcode(data: &[u8], samples: usize, first: usize, out: &mut Vec<u8>) -> Res<()> {
    for i in first..first + samples / SPU_SAMPLES {
        let f = data.get(i * SPU_FRAME..(i + 1) * SPU_FRAME).ok_or("a sound frame is missing")?;
        let shift = f[0] & 0x0F;
        let pred = if f[0] >> 4 > 4 { 0 } else { f[0] >> 4 };
        if shift > 12 {
            return Err(format!("an SPU frame with shift {shift} has no DSPADPCM scale"));
        }
        let head = (pred << 4) | (12 - shift);
        let body: Vec<u8> = f[2..16].iter().map(|&b| (b & 0x0F) << 4 | b >> 4).collect();
        out.push(head);
        out.extend_from_slice(&body[0..7]);
        out.push(head);
        out.extend_from_slice(&body[7..14]);
    }
    Ok(())
}

fn build_bank(head: &[u8], body: &[u8], name: &[u8]) -> Res<Vec<u8>> {
    let bank = String::from_utf8_lossy(name).into_owned();
    let patches = read_bank(head, body, &bank)?;
    let coefs = dsp_coefs();
    let mut recs: Vec<[u8; SBK_PATCH]> = Vec::new();
    let mut blob: Vec<u8> = Vec::new();
    let mut i = 0;
    while i < patches.len() {
        let p = &patches[i];
        let pair = i + 1 < patches.len()
            && patches[i + 1].loop_channel & RIGHT_FLAG != 0
            && p.loop_channel & RIGHT_FLAG == 0
            && patches[i + 1].name == p.name;
        let members = if pair { &patches[i..i + 2] } else { &patches[i..i + 1] };
        i += members.len();
        let looping = p.loop_channel & LOOP_FLAG != 0;
        let mut n = usize::MAX;
        for m in members {
            if m.pan != 0 || m.adsr != 0 {
                return Err(format!("{bank}/{}: Pan or ADSR set; .SBK has nowhere to put them",
                                   String::from_utf8_lossy(m.name)));
            }
            let pcm = decode_adpcm(m.data);
            let len = if looping {
                if pcm.len() < LEAD_IN || pcm[LEAD_IN - 1] != 0 || pcm[LEAD_IN - 2] != 0 {
                    return Err(format!("{bank}/{}: the lead-in leaves a history the DSP would \
                                        not start from", String::from_utf8_lossy(m.name)));
                }
                pcm.len() - LEAD_IN
            } else {
                pcm.len()
            };
            n = n.min(len);
        }
        if n == 0 {
            continue;
        }
        let first = if looping { 1 } else { 0 };
        let mut data: Vec<Vec<u8>> = Vec::new();
        for m in members {
            let mut d = Vec::new();
            transcode(m.data, n, first, &mut d)?;
            data.push(d);
        }
        let channels = members.len();
        let per = data[0].len();
        let mut rec = [0u8; SBK_PATCH];
        let nm = &p.name[..p.name.len().min(31)];
        rec[..nm.len()].copy_from_slice(nm);
        for (k, v) in [bhash(p.name), n as u32, blob.len() as u32, (per * channels) as u32]
            .iter()
            .enumerate()
        {
            rec[0x20 + k * 4..0x24 + k * 4].copy_from_slice(&v.to_le_bytes());
        }
        rec[0x30..0x32].copy_from_slice(&p.pitch.to_le_bytes());
        rec[0x32..0x34].copy_from_slice(&p.volume.to_le_bytes());
        rec[0x34..0x36].copy_from_slice(&p.effect.to_le_bytes());
        rec[0x36] = channels as u8;
        rec[0x37] = looping as u8;
        for c in 0..channels {
            for (k, v) in coefs.iter().enumerate() {
                let o = 0x38 + c * 32 + k * 2;
                rec[o..o + 2].copy_from_slice(&v.to_le_bytes());
            }
        }
        recs.push(rec);
        for d in &data {
            blob.extend_from_slice(d);
        }
    }
    let data_off = SBK_HEADER + SBK_PATCH * recs.len();
    let mut out = Vec::with_capacity(data_off + blob.len());
    for v in [SBK_MAGIC, 1, recs.len() as u32, data_off as u32, blob.len() as u32, bhash(name)] {
        out.extend_from_slice(&v.to_le_bytes());
    }
    let mut short = [0u8; 8];
    let nm = &name[..name.len().min(7)];
    short[..nm.len()].copy_from_slice(nm);
    out.extend_from_slice(&short);
    for r in &recs {
        out.extend_from_slice(r);
    }
    out.extend_from_slice(&blob);
    Ok(out)
}

// ------------------------------------------------------------------- main

fn run(iso: &File, dest_dir: &Path, job: &Job) -> Res<usize> {
    let dest = dest_dir.join(&job.rel);
    match &job.kind {
        Kind::Copy => {
            copy(iso, job.off, job.size, &dest)?;
            Ok(job.size)
        }
        Kind::Expand { out_size } => {
            let out = expand(&read_at(iso, job.off, job.size)?, *out_size)?;
            write_atomic(&dest, &out)?;
            Ok(out.len())
        }
        Kind::Bank { msb_off, msb_size, name } => {
            let head = read_at(iso, job.off, job.size)?;
            let body = read_at(iso, *msb_off, *msb_size)?;
            write_atomic(&dest, &build_bank(&head, &body, name)?)?;
            Ok(0)
        }
    }
}

fn fail(msg: &str) -> ! {
    let mut o = io::stdout().lock();
    let _ = writeln!(o, "error\t{}", msg.replace(['\t', '\n'], " "));
    let _ = o.flush();
    process::exit(2);
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.len() != 3 {
        eprintln!("usage: nfshp2-unpack <disc.iso> <dest dir>  < jobs");
        process::exit(1);
    }
    let iso = Arc::new(File::open(&args[1]).unwrap_or_else(|e| fail(&format!("{}: {e}", args[1]))));
    let dest = Arc::new(PathBuf::from(&args[2]));

    let mut copies = Vec::new();
    let mut work = Vec::new();
    for line in io::stdin().lock().lines() {
        let line = line.unwrap_or_else(|e| fail(&format!("reading the jobs: {e}")));
        if line.is_empty() {
            continue;
        }
        let job = parse(&line).unwrap_or_else(|e| fail(&e));
        match job.kind {
            Kind::Copy => copies.push(job),
            _ => work.push(job),
        }
    }
    work.sort_by(|a, b| b.size.cmp(&a.size));

    let (tx, rx) = mpsc::channel::<Res<(usize, String)>>();
    let cores = thread::available_parallelism().map(|n| n.get()).unwrap_or(4);
    let workers = std::env::var("NFSHP2_UNPACK_THREADS")
        .ok()
        .and_then(|s| s.parse().ok())
        .unwrap_or(cores)
        .max(1);

    {
        let (iso, dest, tx) = (iso.clone(), dest.clone(), tx.clone());
        thread::spawn(move || {
            for job in &copies {
                let r = run(&iso, &dest, job).map(|n| (n, job.rel.clone()));
                let stop = r.is_err();
                let _ = tx.send(r);
                if stop {
                    return;
                }
            }
        });
    }
    let work = Arc::new(work);
    let next = Arc::new(AtomicUsize::new(0));
    for _ in 0..workers {
        let (iso, dest, tx, work, next) = (iso.clone(), dest.clone(), tx.clone(), work.clone(), next.clone());
        thread::spawn(move || loop {
            let i = next.fetch_add(1, Ordering::Relaxed);
            let Some(job) = work.get(i) else { return };
            let r = run(&iso, &dest, job).map(|n| (n, job.rel.clone()));
            let stop = r.is_err();
            let _ = tx.send(r);
            if stop {
                return;
            }
        });
    }
    drop(tx);

    let mut o = io::stdout().lock();
    for r in rx {
        match r {
            Ok((n, rel)) => {
                let _ = writeln!(o, "done\t{n}\t{rel}");
                let _ = o.flush();
            }
            Err(e) => fail(&e),
        }
    }
}
