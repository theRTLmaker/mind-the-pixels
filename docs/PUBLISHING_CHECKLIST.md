# Publishing checklist

Audit of this worktree before publishing it as `mind-the-pixels`
(2026-10-25, updated 2026-10-27). Ticked items were checked; unticked items need a decision.

## Licences

- [x] Upstream `LICENSE` is MIT, Copyright (c) 2026 Zectrix Lab. Kept
      unchanged; redistribution with modifications is allowed as long as the
      notice stays.
- [x] `components/zectrix_epd/LICENSE` is the same MIT licence; its
      `idf_component.yml` declares `license: "MIT"`.
- [x] `THIRD_PARTY_NOTICES.md` and `licenses/TRMNL_FONT_LICENSE.txt` kept.
      The bitmap font is derived from TRMNL16 Regular (SIL OFL 1.1), so the
      OFL text must ship with it.
- [x] `espressif/esp_codec_dev` is downloaded by the component manager into
      the gitignored `managed_components/`; it is not redistributed.
- [x] Demo artwork in `main/assets/` is MIT by Zectrix Lab per the notices.
- [x] README credits upstream and lists what changed.
- [x] Own additions are MIT too: `LICENSE` has a second copyright line,
      `Copyright (c) 2026 Francisco Mendes`, below Zectrix Lab's.
- [x] Open-Meteo (CC BY 4.0) and Darwin (Rail Data Marketplace / Huxley2)
      data notes added to `THIRD_PARTY_NOTICES.md` (2026-10-27).

## Secrets and local files

- [x] `sdkconfig` (contains the Wi-Fi SSID and password) and `sdkconfig.old`
      are gitignored and untracked.
- [x] No credentials, API keys or `/Users/...` paths in tracked or untracked
      source, docs or tools.
- [x] `build/`, `managed_components/`, `simulator/build/` and
      `simulator/out/` are gitignored.
- [x] Original flash backup is outside the repository
      (`~/Documents/Projects/note4/backups/`).
- [x] Local backup `main/app_main.cc.before-layout` moved out of the repo to
      `~/Documents/Projects/note4/backups/worktree-2026-10-25/`.
- [ ] Before the first push, review `git status --short` and
      `git diff --cached --stat`, and run
      `git grep -nI -e PASSWORD -e SSID` to confirm only the Kconfig
      definitions appear.

## Repository

- [x] Main task stack (12288) moved into `sdkconfig.defaults` so a fresh
      clone builds a working configuration.
- [ ] `git remote` still points at `itopinion/zectrix-note4-epd-demo`.
      When creating `mind-the-pixels`, rename that remote to `upstream` and
      add your own as `origin`, keeping the upstream history.
- [x] ESP-IDF project renamed to `mind_the_pixels` (binary
      `build/mind_the_pixels.bin`, 2026-10-27).
- [x] Dashboard Wi-Fi settings moved to the Wi-Fi component's own Kconfig
      (same symbol names, so existing `sdkconfig` values carry over).
