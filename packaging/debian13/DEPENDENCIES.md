# trigger6 1.0.1 - doel: Debian 13 (trixie), kernel 6.12.x LTS

## Pakket `trigger6-dkms` (kernelmodule)
| Dependency | Soort | Toelichting |
|---|---|---|
| dkms (>= 3.0) | Depends | Debian 13: 3.2.2 |
| make, gcc (>= 14) | Depends | Moet bij de kernelcompiler passen (Debian 13: gcc-14) |
| linux-headers-amd64 | Depends/Recommends | Headers voor 6.12.x (bijv. 6.12.107+deb13) |
| kmod | Depends | modprobe/depmod |
| pahole | Recommends | Alleen voor BTF-generatie door kbuild |
| udev (systemd) | Aanbevolen | `71-trigger6.rules` |

Kernelconfig-eisen (standaard in Debian): CONFIG_DRM, CONFIG_DRM_KMS_HELPER, CONFIG_DRM_GEM_SHMEM_HELPER, CONFIG_USB.

## Pakket `trigger6-fast` (gebruikersruimte-helper, optioneel maar nodig voor ~60 fps)
| Dependency | Soort | Toelichting |
|---|---|---|
| libc6 (>= 2.34) | Depends | pthread, dlopen in libc |
| libyuv0 | Depends | Laadt `libyuv.so.0` met dlopen; Debian 13: 0.0.1904.20250204 |
| gcc, make | Build-Depends | Bouwen met `Makefile.helper` |
| systemd | Recommends | gebruikersservice `trigger6-fast.service` |
Zonder helper valt de driver terug op kernelconversie (~29 fps).

## Testfeiten
- Module gecompileerd (zonder fouten of waarschuwingen) tegen Debian `linux-headers-6.12.107+deb13-amd64` op Arch (cross-build). Alleen de BTF-stap faalde door hosttools. Gelinkt: ja.
- Gedraaid en gemeten: alleen Arch 7.2.9, Xorg. Debian 13 zelf is **niet** gedraaid.
- udev-regel en systemd-gebruikersservice: niet op Debian getest.

## 0.9.0-r8 - aanvullende controles
- Gecompileerd tegen Debian `linux-headers-6.12.107+deb13-amd64` (compat-pad `T6_HAVE_DRM_SET_DMA_DEV`), tegen 6.18.55-lts en 7.2.9: zonder fouten/waarschuwingen (BTF-stap alleen niet op de hostmachine).
- Gedraaid: Arch 7.2.9. Gesimuleerde unplug (USB-deauthorisatie) tijdens schrijven: schone disconnect, module vrij, herstel na heraansluiting.
- Niet gedaan: runtime op Debian 13, KASAN/lockdep-kernel.

## 0.9.0-r9 - aanvullende controles
- Gecompileerd tegen 6.18.59 (Gentoo) met `W=1`: zonder fouten of waarschuwingen. Nog niet opnieuw gecompileerd tegen Debian `linux-headers-6.12.107+deb13-amd64` en 7.2.9.
- Geen nieuwe dependencies. De `drm_edid`-API die nu voor de EDID wordt gebruikt, bestaat sinds kernel 6.3.
- Helper getest met libyuv: stopt binnen ongeveer 20 ms op SIGTERM/SIGINT, ook als hij in `read()` wacht.
- Niet gedaan: runtime op de adapter. Zie de testlijst in de changelog van `README.md`.

## 1.0.0 - aanvullende controles
- Vergeleken met de Windows-driver van de leverancier (USB-capture en `T6InDisp.dll`): init- en modusvolgorde, interrupt-endpoint 0x83, eigen modustabel van de adapter en sync-codering. Geen nieuwe dependencies. Nieuwe moduleparameter `keepalive_ms` (0 = uit).
- Gecompileerd tegen Debian 13 (kernel 6.12), Arch Linux (kernel 7.2) en 6.18.59 (Gentoo, `W=1`, zonder fouten of waarschuwingen).
- Getest op de adapter: werkt volledig onder Wayland. Onder Xorg crasht de X-server (SIGSEGV) bij het loskoppelen van de USB-adapter; loskoppelen van alleen de HDMI-kabel werkt wel. Gemeld bij Debian als Xorg-bug.

## 1.0.1 - aanvullende controles
- Geen nieuwe dependencies.
- Gecompileerd tegen 6.18.59 (Gentoo, `W=1`, zonder fouten of waarschuwingen). De wijzigingen gebruiken geen kernelinterfaces naast die van 1.0.0, dat compileert op Debian 13 (kernel 6.12) en Arch Linux (kernel 7.2).
- Getest op de adapter met een Dell P2725HE onder KDE Plasma (Wayland), Gentoo met kernel 6.18.59: beeld, helper, moduswissels, HDMI-hotplug, flip-done-events en een volledige start-up per aansluiting. Zie de changelog in `README.md`.
