"""Verify the public APK asset table and original-map texture fallback."""

import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from tools import embed_assets


def test_public_asset_table_is_empty_but_licensed_fonts_remain_and_compile():
    with tempfile.TemporaryDirectory(prefix="halo-public-assets-") as temporary:
        generated = Path(temporary) / "hud_hires_assets.c"
        subprocess.run(
            [sys.executable, str(ROOT / "tools/embed_assets.py"), str(generated), "--public-apk"],
            cwd=ROOT,
            check=True,
        )
        source = generated.read_text(encoding="utf-8")

        assert "hud_hires_embedded_count = 0;" in source
        assert "static const unsigned int asset" not in source
        assert "text_hires_embedded_count = 3;" in source
        assert '"Overpass-900.ttf"' in source
        assert embed_assets.textures()
        assert not embed_assets.textures(public_apk=True)
        assert any(path.parts[:3] == ("port", "assets", "hud") for path in embed_assets.hud_asset_inputs())
        assert all(path.parts[:3] not in (("port", "assets", "hud"), ("port", "assets", "titles"))
                   for path in embed_assets.hud_asset_inputs(public_apk=True))

        subprocess.run(
            ["clang", "-std=c99", "-fsyntax-only", "-Iport/linux/src", str(generated)],
            cwd=ROOT,
            check=True,
        )


def test_zero_public_table_uses_the_map_texture_when_no_override_exists():
    source = (ROOT / "port/linux/src/hud_hires.c").read_text(encoding="utf-8")
    lookup = source.split("long hud_hires_override_find(", 1)[1].split("int hud_hires_override_coverage", 1)[0]
    assert re.search(r"asset < 0 \|\| asset >= hud_hires_asset_count\(\)\)\s*return -1;", lookup)

    cache = (ROOT / "port/linux/src/xbox_textures.c").read_text(encoding="utf-8")
    result = cache.split("static GLuint texture_entry_result(", 1)[1].split("GLuint xgpu_texture_get(", 1)[0]
    assert "if (entry->override >= 0)" in result
    assert "return entry->texture;" in result

    config = (ROOT / "port/linux/src/port_config.c").read_text(encoding="utf-8")
    assert '#define HIGH_RES_ASSET_DEFAULT "false"' in config
    assert '"display.high_res_hud", _config_boolean, HIGH_RES_ASSET_DEFAULT' in config
    assert '"display.high_res_text", _config_boolean, HIGH_RES_ASSET_DEFAULT' in config


if __name__ == "__main__":
    test_public_asset_table_is_empty_but_licensed_fonts_remain_and_compile()
    test_zero_public_table_uses_the_map_texture_when_no_override_exists()
    print("PASS: public asset generation and zero-table map fallback contracts")
