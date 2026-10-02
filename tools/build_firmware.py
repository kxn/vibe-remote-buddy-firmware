"""Build standalone ESP32-S3 firmware from this repository's own sources."""

import argparse
import os
from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
VARIANTS = ("q2", "o8", "q2-f4")
RELEASE_CONFIG = {
    "CONFIG_BT_NIMBLE_MAX_CONNECTIONS": "2",
    "CONFIG_BT_CTRL_BLE_MAX_ACT": "4",
    "CONFIG_ESP_CONSOLE_NONE": "y",
    "CONFIG_ESP_CONSOLE_SECONDARY_NONE": "y",
    "CONFIG_LOG_DEFAULT_LEVEL_NONE": "y",
    "CONFIG_BOOTLOADER_LOG_LEVEL_NONE": "y",
    "CONFIG_COMPILER_OPTIMIZATION_SIZE": "y",
    "CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_160": "y",
    "CONFIG_FREERTOS_HZ": "1000",
    "CONFIG_PM_ENABLE": "y",
    "CONFIG_PM_DFS_INIT_AUTO": "n",
    "CONFIG_BT_CTRL_MODEM_SLEEP": "y",
    "CONFIG_BT_CTRL_MODEM_SLEEP_MODE_1": "y",
    "CONFIG_BT_CTRL_LPCLK_SEL_MAIN_XTAL": "y",
}
RELEASE_CHOICES = (
    "CONFIG_ESP_CONSOLE_SECONDARY_",
    "CONFIG_LOG_DEFAULT_LEVEL_",
    "CONFIG_BOOTLOADER_LOG_LEVEL_",
    "CONFIG_COMPILER_OPTIMIZATION_",
    "CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ",
    "CONFIG_BT_CTRL_LPCLK_SEL_",
)


def run(args: list[object]) -> None:
    print("+", " ".join(map(str, args)), flush=True)
    subprocess.run(list(map(str, args)), cwd=ROOT, check=True)


def pin_release_config(path: Path) -> None:
    if not path.exists():
        return
    lines = [line for line in path.read_text().splitlines()
             if not any(line.startswith(prefix) or line.startswith("# " + prefix)
                        for prefix in RELEASE_CHOICES)
             and line.removeprefix("# ").removesuffix(" is not set").split("=", 1)[0]
             not in RELEASE_CONFIG]
    path.write_text("\n".join(lines + [f"{k}={v}" for k, v in RELEASE_CONFIG.items()]) + "\n")


def verify_release_config(path: Path) -> None:
    values = dict(line.split("=", 1) for line in path.read_text().splitlines()
                  if line.startswith("CONFIG_") and "=" in line)
    for line in path.read_text().splitlines():
        if line.startswith("# CONFIG_") and line.endswith(" is not set"):
            values[line[2:-11]] = "n"
    wrong = [k for k, v in RELEASE_CONFIG.items() if values.get(k) != v]
    wrong += [k for k in ("CONFIG_LOG_DEFAULT_LEVEL", "CONFIG_BOOTLOADER_LOG_LEVEL")
              if values.get(k) != "0"]
    if values.get("CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG_ENABLED") == "y":
        wrong.append("CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG_ENABLED")
    if wrong:
        raise RuntimeError("Release sdkconfig mismatch: " + ", ".join(wrong))


def project_name(variant: str) -> str:
    return "buddy_s3_q2_f4_ab2" if variant == "q2-f4" else f"buddy_s3_{variant}_ab1"


def build(variant: str) -> None:
    output = ROOT / "build" / f"esp32s3-{variant}"
    pin_release_config(output / "sdkconfig")
    if os.name == "nt":
        idf = Path(os.environ.get("IDF_PATH", str(Path.home() / "esp/esp-idf")))
        if not (idf / "export.ps1").is_file():
            raise RuntimeError("Install ESP-IDF 5.4.x and set IDF_PATH")
        run(["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass",
             "-File", ROOT / "tools/build-idf.ps1", "-IdfPath", idf,
             "-Variant", variant])
    else:
        idf = os.environ.get("IDF_PATH")
        if not idf:
            raise RuntimeError("Source ESP-IDF 5.4.x export.sh first")
        run([sys.executable, Path(idf) / "tools/idf.py", "-C", ROOT / "firmware/esp32s3",
             "-B", output, "-DSDKCONFIG=" + str(output / "sdkconfig"),
             "-DSDKCONFIG_DEFAULTS=" + str(ROOT / "firmware/esp32s3/sdkconfig.defaults")
             + ";" + str(ROOT / f"firmware/esp32s3/sdkconfig.{variant}.defaults"),
             "-DBUDDY_VARIANT=" + variant, "-DS3_HCI_PROBE=OFF",
             "-DS3_USB_QUALIFY=OFF", "-DS3_CODEC_METRICS=OFF", "build"])
    verify_release_config(output / "sdkconfig")
    image = output / f"{project_name(variant)}.bin"
    if not image.is_file():
        raise RuntimeError(f"Build succeeded without image: {image}")
    print(f"Firmware image: {image}", flush=True)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--variant", choices=VARIANTS, help="build one board profile; default: all")
    args = parser.parse_args()
    run([sys.executable, ROOT / "tools/generate_models.py", "--check"])
    for variant in (args.variant,) if args.variant else VARIANTS:
        build(variant)


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, subprocess.CalledProcessError) as exc:
        sys.exit(f"Firmware build failed: {exc}")
