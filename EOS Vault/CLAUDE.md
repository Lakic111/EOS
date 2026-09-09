# EOS — Claude Context File

Lični sistem (Obsidian-stil vault) posvećen predmetu **Ugrađeni/Embedded Operativni
Sistemi (EOS)** — konkretno **4. delu** istog višepredmetnog projekta: pravi Linux
kernel drajver za hardverski akcelerator `ncc_accel` (NCC template matching za
prepoznavanje šahovskih figura) na Zybo ploči (Zynq-7010).

## Pipeline projekta — gde je EOS

| # | Predmet | Šta radi | Status |
|---|---|---|---|
| 1 | PEUSN | ESL/SystemC (TLM) model — referenca | ✅ gotovo (ranije) |
| 2 | PSDS | RTL (ručni VHDL) → sinteza → IP → block design → bitstream → **bare-metal Vitis app** | ✅ **GOTOVO** — FEN tačan na ploči, 32/32, 1.782 ms |
| 3 | FVH | SV/UVM verifikacija istog `ncc_accel` IP-a | ✅ **GOTOVO** — 7/7 koraka, 100% coverage, 27/27 regresija |
| 4 | **EOS** | **Pravi Linux kernel drajver** za `ncc_accel` na istoj ploči/bitstream-u | 🔲 **OVDE SMO** |

Repo/vault-ovi za reference (ne diraj bez pitanja):
- `C:\Users\pc\Desktop\PSDS\PSDS Vault\` — dizajn, RTL, bare-metal app (`src/vitis/app/`)
- `C:\Users\pc\Desktop\FVH\FVH Vault\` — verifikacioni plan i nalazi
- `C:\Users\pc\Desktop\NCC_Akcelerator\` — samostalna, čista kopija repo-a (bitstream,
  XSA, ip_repo sa bare-metal drajverom u `ip_repo/ncc_accel_1_0/drivers/ncc_accel_v1_0/`)

⚠️ **Nema zvaničnog PDF-a sa bodovanjem za EOS** (za razliku od PSDS/FVH). Plan ispod je
izveden iz redosleda vežbi (vidi dole) i iz onoga što je logičan sledeći korak nad istim
hardverom. Kad/ako student dobije zvaničan opis projekta, ovaj fajl i `Projekat/Plan.md`
se ažuriraju da mu odgovaraju.

## Ko sam ja i moja svrha

Student, apsolvent, elektrotehnika. Kroz PSDS i FVH već sam prošao ceo HW tok. Sada
učim Linux kernel/driver stranu: char drajveri, platformski drajveri, napredni kernel
objekti, DMA — i primenjujem to na IP koji sam sâm dizajnirao i verifikovao.

## Hardverski ugovor — MERODAVNO (izvučeno iz PSDS/BUGS.md i Koraka 9 dizajna)

### Registri, S00 (AXI-Lite, po instanci, 4K opseg)

| Offset | Registar | Širina | Napomena |
|---|---|---|---|
| `0x00` | `IMG_W` | 8 bita | |
| `0x04` | `IMG_H` | 8 bita | |
| `0x08` | `TMP_W` | 8 bita | |
| `0x0C` | `TMP_H` | 8 bita | |
| `0x10` | `IMG_ADDR` | — | rezervisan, bez značenja |
| `0x14` | `TMP_ADDR` | — | rezervisan, bez značenja |
| `0x30` | `CTRL` | bit0 | upis 1 → start, briše `done_sticky` |
| `0x34` | `STATUS` | bit0/bit1 | bit0=`done_sticky` (read-only), bit1=`busy` |

**Nema IRQ porta na IP-u — isključivo prozivanje (polling).** Ovo je centralna
projektantska odluka za drajver (vidi "Otvorena pitanja" dole).

### Memorije, S01 (AXI-Full, po instanci, 128K opseg)

| Region | Offset | Kapacitet | Korišćenje |
|---|---|---|---|
| slika | `+0x00000` | 32 KB (8192 reči) | do 90×90 = 8100 reči |
| šablon | `+0x08000` | 32 KB (8192 reči) | do 30×30 = 900 reči |
| rezultat | `+0x10000` | 64 KB (16384 reči) | do 3721 reči (fino) |

Jedan piksel = jedna 32-bitna reč. `IMG_ADDR`/`TMP_ADDR` se ignorišu — jezgro uvek čita
sa fiksnih offseta.

### Adresna mapa (fizičke adrese na PS `M_AXI_GP0`)

| Blok | S00 (kontrola) | S01 (memorije) |
|---|---|---|
| `ncc0` | `0x5000_0000` | `0x5002_0000` |
| `ncc1` | `0x5100_0000` | `0x5102_0000` |
| `axi_cdma_0` | `0x6000_0000` (S_AXI_LITE) | — |

### ⚠️ Poznato hardversko ograničenje — KRITIČNO za dizajn DMA sloja

**Burst duži od 2 beata kroz `axi_interconnect_0` blokira celu PL magistralu**
(`axi_interconnect_0` je `STRATEGY=1`, deljena magistrala zbog tajminga). Dokazano na
pravom hardveru preko JTAG-a (PSDS `BUGS.md`), nezavisno od softvera. Bare-metal Vitis
aplikacija je zato **odustala od CDMA-a** i prenosi sve procesorom (`Xil_Out32`/`In32`
petlje) — košta ~9% vremena, ali radi pouzdano.

**Posledica za EOS drajver:** ako se ide na `dmaengine` klijent za `axi_cdma_0`, MORA se
ili (a) ograničiti na transfere ≤ 2 beata (nepraktično za slike od 8100 reči), ili
(b) prihvatiti istu odluku kao bare-metal i **ne koristiti DMA za same podatke** —
umesto toga demonstrirati DMA veštinu (vežbe 11-12) na nivou koncepta/manjeg primera,
ili predložiti popravku (izmeštanje AXI-Lite slave-ova na zaseban interkonekt — PSDS
BUGS.md pominje kao nedovršenu hipotezu) ako student želi da je stvarno reši. Ovo je
**otvoreno pitanje**, videti dole.

### Drugi invarijant iz bare-metal iskustva

- `done_sticky` se NE briše čitanjem, samo sledećim `CTRL` upisom.
- Skorovi se porede kao **`u32`** ne `int32` (`0x80000000` = savršeno poklapanje, a kao
  signed je negativan) — ako drajver ili userspace aplikacija ponovo implementira
  poređenje, mora se čuvati ovaj invarijant.
- AXI-Lite S00 je imao bug (upis visi ako `W` stigne pre `AW`) — **popravljen u RTL-u**
  pre nego što je IP spakovan za FVH/EOS upotrebu; ne bi trebalo da se vidi sa PS strane
  (PS uvek šalje AW pre W), ali ako drajver ikad radi sirov AXI bring-up preko JTAG-a i
  vidi zamrzavanje, ovo je poznat istorijski uzrok.

## Vežbe (materijal na Desktop-u, van vault-a) — mapiranje na zadatak

| Vežba | Tema | Primena na `ncc_accel` |
|---|---|---|
| Vezba2 (GCC/Make) | build alati | Makefile za drajver (kernel module build sistem) |
| Vezba3 (Aplikacije) | Linux aplikacije | userspace app koja poziva drajver |
| Vezba5-6 (sekvencijalni uređaji) | char drajveri, file_operations | osnova za `/dev/ncc0`, `/dev/ncc1` |
| Vezba7 (napredni kernel objekti) | wait queue, kfifo, sinhronizacija | čekanje na `done` bez IRQ-a (polling u kernelu + `wait_event`) |
| Vezba9-10 (platformski uređaji) | `platform_driver`, device tree, `ioremap` | osnovni skelet drajvera za `ncc_accel` |
| Vezba11-12 (VGA BRAM/DMA kontroler, Linux na Zybo) | mmap BRAM-a, DMA kontroler, instalacija Linuxa na ploču | mmap S01 regiona ka userspace-u; DMA odluka (gore); boot Linuxa na istom Zybo/bitstream-u kao PSDS |

## Moja pravila i granice

- **Direktno i bez uvijanja** — reci kad nešto neće raditi ili nije dovoljno za projekat.
- **Označavaj svoje fajlove** — sve što generišem ide sa prefiksom `(C)`, isto kao PSDS/FVH.
- **Ne diraj PSDS/FVH kod** — čitaj ih samo kao referencu (registri, bring-up redosled,
  bare-metal `ncc_hw.c`/`ncc_app.c` kao logička referenca za port).
- **Nema izmišljanja bodovanja** — pošto nema zvaničnog PDF-a, ne tvrditi da nešto "nosi
  X bodova"; koristiti "korak" bez brojeva dok student ne donese zvaničan opis.

## Folder struktura (predlog)

```
EOS Vault/
├── CLAUDE.md                  ← Ovde si
└── Projekat/
    ├── Plan.md                 ← Plan implementacije po koracima
    └── Napredak.md             ← Status/odluke, ažurira se usput
```

Kod (kad počne) ide van vault-a, npr. `C:\Users\pc\Desktop\EOS\driver\` (kernel modul),
`C:\Users\pc\Desktop\EOS\app\` (userspace), po uzoru na `PSDS/src/vitis/app/` strukturu.

## Odluke (Korak 1) — rešeno 2026-09-06

1. **Build okruženje**: **PetaLinux**, alat se instalira u postojećoj **VirtualBox
   Ubuntu VM** (host: Windows). Ploča se boot-uje sa SD kartice na pravoj Zybo ploči
   (ne QEMU/simulacija); `.xsa` se uvozi preko `petalinux-create --type project
   --template zynq` pa `petalinux-config --get-hw-description=<put do .xsa>`, koristi
   se najsvežiji: `PSDS/src/vitis/ws/ncc_plat/export/ncc_plat/hw/ncc_system_wrapper.xsa`.
   **Verzija PetaLinux-a MORA odgovarati Vivado verziji kojom je `.xsa` generisan
   (Vivado 2025.2, iz PSDS `CLAUDE.md`)** — proveriti tačnu Ubuntu verziju u VM-u i
   uskladiti sa zvaničnom podržanom listom za tu PetaLinux verziju (UG1144 release
   notes za odgovarajuću godinu/release).
2. **DMA**: **Opcija A** — bez DMA za same podatke (isto kao bare-metal), zbog
   burst>2-beat ograničenja na `axi_interconnect_0`. DMA nastavni cilj (vežbe 11-12)
   se ispunjava malim/sintetičkim `dmaengine` primerom koji ne udara u limit, ne
   prenosom slike/šablona/rezultata.
3. **Interfejs ka userspace-u**: **ioctl + mmap**. `ioctl` samo za kontrolu
   (`NCC_SET_DIMS`, `NCC_START`, `NCC_WAIT_DONE`); `mmap` direktno mapira S01 region
   (128K) u userspace — bez `copy_to/from_user`. Keš koherencija se rešava analogno
   `Xil_DCacheFlushRange`/`Xil_DCacheInvalidateRange` (npr. `dma_alloc_coherent` ili
   eksplicitan flush/invalidate na mapiranom regionu).
4. **Jedan vs dva drajvera**: **dva nezavisna device node-a**, `/dev/ncc0` i `/dev/ncc1`.
   Svaka instanca ima svoj `probe()`, svoj `cdev`, potpuno nezavisni. Paralelizam
   (ako se radi) je na nivou userspace aplikacije, ne hardvera/drajvera — isto kao
   bare-metal, gde `ncc0`/`ncc1` nisu hardverski povezani.
