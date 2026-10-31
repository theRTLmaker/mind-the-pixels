#!/usr/bin/env python3
"""Battery model for Mind the Pixels on the ZECTRIX NOTE4.

Estimates daily consumption and battery life for the current firmware and
for candidate optimisations, so they can be compared before building them.

Every current is an ESTIMATE (typical ESP32-S3 / e-paper figures), not a
measurement of this board. Replace the values in `Params` with measured ones
(see "Calibrating" in the output) and rerun:

    python3 tools/power_model.py
"""

from dataclasses import dataclass, replace

DAY_HOURS = 17      # 05:00-22:00 awake
NIGHT_HOURS = 7     # 22:00-05:00 deep sleep with hourly wakes


@dataclass(frozen=True)
class Params:
    # Battery: 2000+ mAh per Zectrix. Usable capacity between 4.2 V and the
    # firmware's 0 % (~3.4 V) is a bit lower, and drops as the cell ages.
    battery_mah: float = 2000
    usable_fraction: float = 0.90

    # Board idle while awake, between events (mA). Without power management
    # the CPU spins at 160 MHz and Wi-Fi stays in modem sleep: ~30-40 mA is
    # typical for an ESP32-S3 module with PSRAM. Auto light sleep keeps Wi-Fi
    # associated (waking for DTIM beacons) at ~2-4 mA.
    idle_ma: float = 35.0
    # "Audio" rail (ES8311 codec + amplifier). It also powers the I2C bus
    # (RTC, NFC): every I2C transfer switches it on, so it stays on.
    audio_rail_ma: float = 3.0

    # HTTPS request: TLS handshake + transfer at Wi-Fi active current.
    request_ma: float = 90.0
    request_s: float = 1.8
    # Commute fetch = three streamed boards.
    commute_requests: int = 3

    # E-paper: panel + boost converter during a refresh (mA, s).
    partial_ma: float = 20.0
    partial_s: float = 0.8
    gray_ma: float = 25.0
    gray_s: float = 7.2

    # Deep sleep (board level: ESP32-S3 RTC, regulator, charger, RTC chip,
    # pull-ups). The bare chip is ~10 uA; boards are usually 50-300 uA.
    deep_sleep_ma: float = 0.15
    # Night wake: boot + Wi-Fi connect, fetch weather and picks, gray refresh.
    wake_boot_ma: float = 80.0
    wake_boot_s: float = 4.0

    # How the day is spent.
    train_fetch_per_h: float = 60       # dashboard train card, every minute
    clock_partials_per_h: float = 60    # clock (+ battery) every minute
    gray_refresh_per_h: float = 3       # weather/picks changes, partial limit
    slow_fetch_per_h: float = 4 * 2     # weather + events every 15 min
    commute_fetch_per_h: float = 0      # only while the commute screen shows
    night_wakes: float = NIGHT_HOURS    # hourly night refresh


def mah(ma: float, seconds: float) -> float:
    return ma * seconds / 3600.0


def daily_budget(p: Params) -> dict:
    """mAh per day, split by what uses it."""
    base = (p.idle_ma + p.audio_rail_ma) * DAY_HOURS
    trains = mah(p.request_ma, p.request_s) * p.train_fetch_per_h * DAY_HOURS
    slow = mah(p.request_ma, p.request_s) * p.slow_fetch_per_h * DAY_HOURS
    commute = (mah(p.request_ma, p.request_s) * p.commute_requests *
               p.commute_fetch_per_h * DAY_HOURS)
    display = (mah(p.partial_ma, p.partial_s) * p.clock_partials_per_h +
               mah(p.gray_ma, p.gray_s) * p.gray_refresh_per_h) * DAY_HOURS
    night_wake = (mah(p.wake_boot_ma, p.wake_boot_s) +
                  mah(p.request_ma, p.request_s) * 2 +
                  mah(p.gray_ma, p.gray_s))
    night = p.deep_sleep_ma * NIGHT_HOURS + night_wake * p.night_wakes
    return {
        "awake idle": base,
        "train fetches": trains,
        "weather + picks": slow,
        "commute fetches": commute,
        "display refreshes": display,
        "night": night,
    }


def days(p: Params) -> float:
    return p.battery_mah * p.usable_fraction / sum(daily_budget(p).values())


# Changes, each applied on top of the previous one. The first three are
# in the firmware since 2026-10-31.
STEPS = [
    ("Before power management", lambda p: p),
    ("[done] Auto light sleep (CPU sleeps, Wi-Fi stays connected)",
     lambda p: replace(p, idle_ma=3.0)),
    ("[done] Trains every minute 06-10 and 16-20, every 5 min otherwise",
     lambda p: replace(p, train_fetch_per_h=(8 * 60 + 9 * 12) / DAY_HOURS)),
    ("Clock to a 5-minute display update outside those hours",
     lambda p: replace(p, clock_partials_per_h=(8 * 60 + 9 * 12) / DAY_HOURS)),
]

# Alternatives, compared against the light-sleep firmware.
ALTERNATIVES = [
    ("No night sleep (stay awake 24 h, light sleep)",
     lambda p: replace(p, deep_sleep_ma=3.0 + 0.0, night_wakes=0)),
    ("Night screen without hourly weather",
     lambda p: replace(p, night_wakes=1)),
    ("Commute screen left on all day (3 boards / 2 min)",
     lambda p: replace(p, commute_fetch_per_h=30)),
]


def main() -> None:
    p = Params()
    print("Mind the Pixels battery model (estimates, see Params)\n")
    print(f"Battery {p.battery_mah:.0f} mAh, {p.usable_fraction:.0%} usable; "
          f"awake {DAY_HOURS} h, night {NIGHT_HOURS} h\n")

    print("Where today's charge goes (mAh per day):")
    budget = daily_budget(p)
    total = sum(budget.values())
    for name, value in sorted(budget.items(), key=lambda kv: -kv[1]):
        print(f"  {name:20s} {value:7.1f}  {value / total:5.1%}")
    print(f"  {'total':20s} {total:7.1f}  -> {days(p):.1f} days\n")

    print("Optimisations, cumulative:")
    current = p
    for name, change in STEPS:
        current = change(current)
        per_day = sum(daily_budget(current).values())
        print(f"  {days(current):5.1f} days  {per_day:6.1f} mAh/day  {name}")
    print()

    light = STEPS[1][1](p)
    print("Alternatives (vs. light sleep, "
          f"{days(light):.1f} days):")
    for name, change in ALTERNATIVES:
        print(f"  {days(change(light)):5.1f} days  {name}")
    print()

    print("Calibrating: measure the board on USB with a USB power meter once")
    print("the battery is full (charge LED off), then set idle_ma, request_ma")
    print("and gray_ma from the readings and rerun.")


if __name__ == "__main__":
    main()
