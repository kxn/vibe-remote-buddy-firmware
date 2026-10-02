"""Ensure cached sdkconfig values cannot silently disable release power saving."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location(
    "build_firmware", Path(__file__).resolve().parents[1] / "tools/build_firmware.py")
build = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(build)


class ReleasePowerConfigTest(unittest.TestCase):
    def test_existing_disabled_settings_and_clock_choices_are_replaced(self):
        with tempfile.TemporaryDirectory() as directory:
            config = Path(directory) / "sdkconfig"
            config.write_text(
                "# CONFIG_PM_ENABLE is not set\n"
                "CONFIG_PM_DFS_INIT_AUTO=y\n"
                "# CONFIG_BT_CTRL_MODEM_SLEEP is not set\n"
                "CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y\n"
                "CONFIG_BT_CTRL_LPCLK_SEL_RTC_SLOW=y\n"
                "CONFIG_SPIRAM_SPEED_80M=y\n")
            build.pin_release_config(config)
            text = config.read_text()
            self.assertNotIn("# CONFIG_PM_ENABLE is not set", text)
            self.assertNotIn("# CONFIG_BT_CTRL_MODEM_SLEEP is not set", text)
            self.assertNotIn("CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y", text)
            self.assertNotIn("CONFIG_BT_CTRL_LPCLK_SEL_RTC_SLOW=y", text)
            for expected in ("CONFIG_PM_ENABLE=y", "CONFIG_PM_DFS_INIT_AUTO=n",
                             "CONFIG_BT_CTRL_MODEM_SLEEP=y", "CONFIG_BT_CTRL_LPCLK_SEL_MAIN_XTAL=y",
                             "CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_160=y", "CONFIG_SPIRAM_SPEED_80M=y"):
                self.assertIn(expected + "\n", text)
            build.pin_release_config(config)
            self.assertEqual(config.read_text(), text)

    def test_verification_reads_kconfig_disabled_syntax_and_rejects_no_pm(self):
        with tempfile.TemporaryDirectory() as directory:
            config = Path(directory) / "sdkconfig"
            # Mimic Kconfig's serialized output, where n uses a comment.
            lines = [f"# {key} is not set" if value == "n" else f"{key}={value}"
                     for key, value in build.RELEASE_CONFIG.items()]
            lines += ["CONFIG_LOG_DEFAULT_LEVEL=0", "CONFIG_BOOTLOADER_LOG_LEVEL=0"]
            valid = "\n".join(lines) + "\n"
            config.write_text(valid)
            build.verify_release_config(config)
            config.write_text(valid.replace("CONFIG_PM_ENABLE=y", "# CONFIG_PM_ENABLE is not set"))
            with self.assertRaisesRegex(RuntimeError, "CONFIG_PM_ENABLE"):
                build.verify_release_config(config)


if __name__ == "__main__":
    unittest.main()
