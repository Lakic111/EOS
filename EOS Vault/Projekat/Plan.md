---
tags: [eos, projekat, plan]
---

# Plan implementacije — EOS Linux drajver za NCC akcelerator

Cilj: napraviti pravi Linux kernel platform drajver (+ userspace aplikaciju) za
`ncc_accel` IP na Zybo ploči, koristeći **isti bitstream/XSA** koji je PSDS već doveo do
rada u bare-metal Vitis-u. Ovo NIJE redizajn hardvera — RTL i adresna mapa su fiksni
(videti `../CLAUDE.md`, odeljak "Hardverski ugovor").

Pošto nema zvaničnog opisa/bodovanja EOS projekta, koraci ispod su izvedeni iz redosleda
EOS vežbi i iz logičnog nastavka PSDS/FVH rada. **Ažurirati čim student dobije zvaničan
opis projekta** — brojevi/opseg koraka će se verovatno promeniti.

---

## Korak 1 — Odluke i priprema okruženja

Odluke (videti `CLAUDE.md`, "Odluke (Korak 1)", rešeno 2026-09-06):
- **PetaLinux**, boot na pravoj Zybo ploči
- **DMA**: bez DMA za podatke (Opcija A, isto kao bare-metal); DMA cilj kroz mali primer
- **Interfejs**: ioctl (kontrola) + mmap (S01 region direktno)
- **Dva nezavisna device node-a**: `/dev/ncc0`, `/dev/ncc1`

Preostaje praktični deo: instalirati/podesiti PetaLinux, uvesti
`ncc_system_wrapper.xsa` (iz `PSDS/src/vhdl/result/` ili `NCC_Akcelerator/release/`),
generisati device tree sa PS perifernim uređajima, boot-ovati Linux na Zybo (originalni,
`zybo:part0:2.0`) preko istog bitstream-a koji već radi u bare-metal-u. Kriterijum:
Linux prompt na UART-u (isti COM port/baud kao bare-metal, 115200).

## Korak 2 — Device tree čvor za `ncc_accel`

Dodati device tree node(s) za `ncc0`/`ncc1` sa `compatible = "xlnx,ncc-accel"` (ili
slično), `reg` sa oba opsega (S00 4K na `0x50000000`/`0x51000000`, S01 128K na
`0x50020000`/`0x51020000`). Bez `interrupts` property (IP nema IRQ).

**Izlaz:** `.dts`/`.dtsi` fragment, potvrđen da se čvor pojavljuje u
`/proc/device-tree/` ili `/sys/firmware/devicetree/base/` posle boot-a.

## Korak 3 — Osnovni platform drajver (skelet)

Iz vežbi 9-10: `platform_driver` sa `probe`/`remove`, `of_match_table` po
`compatible` string-u, `ioremap` oba opsega (S00, S01) po instanci u probe-u.

Minimalna provera: `dev_info` ispiše fizičke/virtuelne adrese pri probe-u; modul se
učitava (`insmod`) i uklanja (`rmmod`) bez greške; `platform_driver` se vezuje za oba
device tree čvora (`ncc0`, `ncc1`).

## Korak 4 — Sysfs interfejs (najjednostavniji nivo kontrole)

Sysfs atributi (`IMG_W`, `IMG_H`, `TMP_W`, `TMP_H`, `start`, `status`) preko
`DEVICE_ATTR` — omogućava ručno testiranje sa `cat`/`echo` iz shell-a pre nego što se
piše bilo kakav userspace C kod. Dobra provera da AXI-Lite put radi (isto kao Faza 1
bare-metal bring-upa: `STATUS` čitljiv, upis `IMG_W` pa čitanje vrati istu vrednost).

## Korak 5 — Char device: ioctl kontrola + mmap podataka

Prava upravljačka putanja za userspace aplikaciju:
- `/dev/ncc0`, `/dev/ncc1` (ili `/dev/ncc_accel0`/`1`) preko `cdev`/`misc_register`
- `ioctl`: `NCC_SET_DIMS`, `NCC_START`, `NCC_WAIT_DONE` (blokira dok `busy=0`/
  `done_sticky=1`, sa timeout-om — isto obrazloženje kao bare-metal `ncc_wait_done`:
  bez timeout-a svaka konfiguraciona greška izgleda kao zamrznut sistem)
- `mmap`: mapira S01 region (128K) direktno u userspace, tako da aplikacija piše
  sliku/šablon i čita rezultate bez `copy_to/from_user` — analogno vežbi 11 (mmap BRAM-a)

**Sinhronizacija bez IRQ-a** (vežba 7): `NCC_WAIT_DONE` u kernelu ili polluje STATUS u
petlji sa `usleep_range`/`schedule_timeout`, ili se implementira kao "userspace polluje
sam" — odluka iz Koraka 1 određuje ovo.

**Izlaz:** modul se učitava, `mmap` + `ioctl` iz malog test programa upisuje/pokreće/
čita nazad poznatu vrednost iz jezgra (npr. ponoviti Fazu 3 bare-metal bring-upa: golden
90×90 + crni top 25×15 → `0x80000000 @ (32,14)`).

## Korak 6 — Port userspace aplikacije (`ncc_app`/`ncc_hw` → Linux)

Prevesti `PSDS/src/vitis/app/ncc_hw.c` + `ncc_app.c` sa `Xil_Out32`/`Xil_In32` na
`open`/`ioctl`/`mmap` poziv drajvera iz Koraka 5. Logika skeniranja 8×8 table,
grubi-pa-fini screen, FEN generisanje — **ne menja se**, samo sloj ka hardveru.
Čuvati invarijant: skorovi kao `u32` (ne `int32`), isto poravnanje (4B) za mmap-ovane
regione.

**Podaci:** isti `board2.txt` + 12 šablona, sada kao fajl na Linux fajl-sistemu (SD
kartica/rootfs) umesto `const` niza u `.elf` — prva prilika da se pokaže prednost
Linux-a nad bare-metal-om (učitavanje sa diska, ne mora se rekompajlirati za novu sliku).

**Kriterijum prihvatanja:** FEN sa Linux aplikacije **identičan** bare-metal/FVH/PSDS
rezultatu — `rnbqkbnr/pp5p/4ppp1/2pp4/5P2/1P1BPN2/P1PPQ1PP/RNB1K2R`, 32/32 polja.

## Korak 7 — DMA (ako odluka iz Koraka 1 kaže da se radi)

Ako se ide na `dmaengine` klijent za `axi_cdma_0`: poštovati ograničenje iz `BUGS.md`
(burst > 2 beata kroz `axi_interconnect_0` blokira magistralu) — testirati inkrementalno
(1, 2, 3 beat-a) na pravom hardveru pre nego što se veruje bilo kojoj količini podataka,
tačno kako je PSDS to izmerio direktno preko JTAG-a. Ako ograničenje i dalje postoji,
ili se DMA ograničava na bezbedan opseg (nepraktično malo), ili se dokumentuje ista
odluka kao bare-metal (bez DMA za podatke), uz demonstraciju DMA API-ja na manjem/
sintetičkom primeru radi ispunjenja nastavnog cilja vežbi 11-12.

## Korak 8 — Merenje i poređenje

Izmeriti vreme celog skeniranja (8×8) sa Linux aplikacijom, uporediti sa bare-metal
brojkom (**1.782 ms**, PSDS Korak 9). Očekivano: Linux dodaje overhead (syscall/ioctl
latencija, keš/MMU), ali PL računanje (87% vremena u bare-metal-u) ostaje dominantno pa
razlika ne bi trebalo da bude drastična — ako jeste, istražiti zašto (loša mmap
konfiguracija, nepotreban copy, cache flush na pogrešnom mestu).

## Korak 9 — Dokumentacija

Isti stil kao PSDS/FVH: arhitektura drajvera (dijagram: userspace app → ioctl/mmap →
platform driver → AXI-Lite/AXI-Full), tabela registara, DMA odluka i obrazloženje,
merenje iz Koraka 8, poznata ograničenja.

---

## Status

> Ažurirati posle svake sesije.

- 2026-09-06: Sve 4 odluke iz Koraka 1 donete (PetaLinux / bez DMA za podatke / ioctl+mmap
  / dva nezavisna device node-a). Sledeće: praktični deo Koraka 1 — instalacija PetaLinux-a,
  uvoz `ncc_system_wrapper.xsa`, boot Linuxa na Zybo ploči preko UART-a.
- 2026-09-09: Provera VM-a (VirtualBox, Ubuntu 22.04.5, `stefan-VirtualBox`) — PetaLinux
  2025.2 već instaliran u `/home/stefan/petalinux` (poklapa se sa Vivado 2025.2 po UG1144),
  `settings.sh` radi, svi preduslovi prisutni (gcc/g++/make/python3/git). `.xsa` je već na
  VM-u: `/home/stefan/EOS/ncc_system_wrapper.xsa`. Sledeći praktični korak: `petalinux-create
  --type project --template zynq` pa `petalinux-config --get-hw-description=...xsa`.
  ⚠️ **Resursno usko grlo pre `petalinux-build`**: disk 39G slobodno (Yocto build zna da
  uzme 40-100GB) — obrisati installer (`petalinux-v2025.2-...-installer.run`, 3.1GB) i/ili
  proširiti disk; RAM samo 3.8GB+2.6GB swap (Xilinx traži min 8GB, realno 16GB za Yocto) —
  build će raditi ali sporo, razmisliti o povećanju RAM-a VM-a i ograničavanju
  `BB_NUMBER_THREADS`/`PARALLEL_MAKE`.
  **Odluka**: za sada NE povećavati RAM/disk VM-a (host ima samo 15.3GB ukupno, 1.2GB
  trenutno slobodno — nema prostora da se izdvoji 6-8GB VM-u bez guranja hosta u swap).
  Nastavlja se sa trenutnim resursima (3.8GB RAM), obrisati installer radi mesta na disku,
  videti kako ide `petalinux-build` pa eventualno revidirati.
- 2026-09-09 (nastavak): **Korak 1 praktično gotov do build-a.** Installer obrisan (disk
  42G/74G slobodno). Projekat kreiran: `/home/stefan/EOS/ncc_plat`
  (`petalinux-create project -n ncc_plat --template zynq` — napomena: u 2025.2 sintaksa je
  promenjena, `--type project` više ne postoji, sad su subkomande). Hardver uvezen
  (`petalinux-config --get-hw-description=.../ncc_system_wrapper.xsa --silentconfig`,
  bez GUI-ja jer VM nema TTY) — SoC ispravno prepoznat kao `zynq-generic-7z010` (Zybo
  Z7-10), potvrđuje da je `.xsa` dobar. `CONFIG_SUBSYSTEM_MACHINE_NAME="template"` za sada
  (generički template, ne pravi Digilent board fajl — može se promeniti kasnije ako треба).
  Projekat 184MB. `petalinux-build` NIJE pokrenut.
  Sledeće: pre build-a ograničiti `BB_NUMBER_THREADS`/`PARALLEL_MAKE` u
  `build/conf/local.conf` (RAM je usko grlo, 3.8GB), zatim `petalinux-build` da se generiše
  `pl.dtsi` sa `ncc_accel` node-om (tek tada se vidi tačan `compatible`/`reg` koji drajver
  koristi — device tree fragment za ncc_accel se generiše iz Vivado IP metapodataka tokom
  build-a, ne postoji ranije).
- 2026-09-09 (nastavak 2): VM potvrdio hardverski ugovor direktno parsiranjem
  `ncc_system.hwh` iz `.xsa` — adrese se poklapaju 1:1 sa `CLAUDE.md` (ncc0 S00
  0x50000000-0xFFF, S01 0x50020000-0x5003FFFF/128KB; ncc1 S00 0x51000000-0xFFF, S01
  0x51020000-0x5103FFFF/128KB) i **potvrđeno nema interrupt port** (ni SIGIS=interrupt ni
  intr/irq u imenima portova, samo s00_axi_*/s01_axi_*). VM resursi: 6 CPU jezgara (Ryzen 7
  7445HS), 3.8GB RAM (~2.2GB available), 2.6GB swap, 42GB disk slobodno.

  **Plan za build večeras (pripremljene komande, još nije pokrenuto):**
  1. U `~/EOS/ncc_plat/build/conf/local.conf` dodati/podesiti:
     ```
     BB_NUMBER_THREADS = "2"
     PARALLEL_MAKE = "-j 2"
     INHERIT += "rm_work"
     ```
     I promeniti disk-monitor HALT sa 100M na 2G (linija sa `BB_DISKMON_DIRS`, obično već
     postoji u local.conf sa HALT vrednošću — promeniti na 2G umesto default 100M).
  2. Pokrenuti build u pozadini (ne uživo, satima traje):
     ```
     source ~/petalinux/settings.sh
     cd ~/EOS/ncc_plat
     nohup petalinux-build > ~/EOS/build_$(date +%Y%m%d_%H%M).log 2>&1 &
     ```
  3. Procena trajanja: 4-8h na ovim resursima (ograničeno na -j2 zbog RAM-a, ne CPU-a).
     Pustiti preko noći, ne čekati uživo.
  4. Ujutru proveriti: `tail -100 ~/EOS/build_*.log`, exit status procesa, i da li postoji
     `pl.dtsi` sa `ncc_accel` node-om (verovatno u
     `components/plnx_workspace/device-tree/device-tree/pl.dtsi` ili sličnoj putanji unutar
     `ncc_plat/`) — uporediti sa gornjim adresama, treba da NEMA `interrupts` property.
  Sledeće nakon uspešnog builda: Korak 2 (device tree čvor za ncc_accel — verovatno već
  auto-generisan iz Vivado IP-a, samo proveriti/prilagoditi `compatible` string), pa Korak
  3 (platform driver skelet).
