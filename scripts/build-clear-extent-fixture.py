"""Build private clear-shader inputs from a decoded game executable."""

import argparse
import hashlib
from pathlib import Path
import struct


ROOT = Path(__file__).resolve().parent.parent
SHADER_WORDS = 27
SHADER_DIGEST = "abfec28e9858fd1b9b27afd527f08421c38f288e9333cc7fc10d6391ac190fa9"


def find_shader(image):
    size = SHADER_WORDS * 4
    for offset in range(0, len(image) - size + 1, 4):
        candidate = image[offset:offset + size]
        if hashlib.sha256(candidate).hexdigest() == SHADER_DIGEST:
            return struct.unpack(">" + str(SHADER_WORDS) + "I", candidate)
    raise ValueError("The decoded executable does not contain the supported D3D clear shader")


def float_bits(value):
    return struct.unpack(">I", struct.pack(">f", value))[0]


def clear_input(width, height):
    registers = {
        "VGT_DRAW_INITIATOR": 0x00030088,
        "VGT_INDX_OFFSET": 0,
        "VGT_MIN_VTX_INDX": 0,
        "VGT_MAX_VTX_INDX": 0xFFFF,
        "VGT_DMA_BASE": 0,
        "VGT_DMA_SIZE": 0,
        "PA_CL_VTE_CNTL": 0x300,
        "PA_CL_CLIP_CNTL": 0x10000,
        "PA_SU_VTX_CNTL": 4,
        "PA_SU_SC_MODE_CNTL": 0x10000,
        "PA_CL_VPORT_XSCALE": float_bits(width / 2),
        "PA_CL_VPORT_XOFFSET": float_bits(width / 2),
        "PA_CL_VPORT_YSCALE": float_bits(-height / 2),
        "PA_CL_VPORT_YOFFSET": float_bits(height / 2),
        "PA_CL_VPORT_ZSCALE": float_bits(-1),
        "PA_CL_VPORT_ZOFFSET": float_bits(1),
        "PA_SC_WINDOW_OFFSET": 0,
        "PA_SC_WINDOW_SCISSOR_TL": 0,
        "PA_SC_WINDOW_SCISSOR_BR": 0x20002000,
        "PA_SC_SCREEN_SCISSOR_TL": 0,
        "PA_SC_SCREEN_SCISSOR_BR": 0x20002000,
        "RB_SURFACE_INFO": (width + 31) & ~31,
        "PA_SC_AA_MASK": 0xFFFF,
        "SQ_CONTEXT_MISC": 0,
        "RB_DEPTH_INFO": 0,
        "RB_COLORCONTROL": 0,
        "RB_ALPHA_REF": 0,
        "RB_STENCILREFMASK": 0xFF0000,
        "RB_STENCILREFMASK_BF": 0xFFFF00,
        "SQ_PROGRAM_CNTL": 0x10010001,
        "SQ_VS_CONST": 0xFF000,
        "SQ_PS_CONST": 0xFF100,
    }
    vertices = bytearray()
    for x, y in ((-0.5, -0.5), (width - 0.5, -0.5), (width - 0.5, height - 0.5)):
        vertices.extend(struct.pack(">7f", x, y, 0, 0, 0, 0, 0))
    entries = ", ".join(
        "{rex::graphics::XE_GPU_REG_" + name + f", 0x{value:08X}u}}"
        for name, value in registers.items()
    )
    return "    {{ {" + entries + "} }, {" + ", ".join(map(str, vertices)) + "}, " + str(height) + "},"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", type=Path, default=ROOT / "private/image.bin")
    parser.add_argument("--output", type=Path, default=ROOT / "private/clear_extent_fixture.h")
    args = parser.parse_args()
    shader = find_shader(args.image.read_bytes())
    dimensions = [(512, 256)]
    dimensions.extend(
        (width, height)
        for width in (64, 128, 256, 512, 640, 1280, 1920, 2048)
        for height in (1, 16, 128, 256)
        if (width, height) != (512, 256)
    )
    lines = [
        "// Generated locally from the game executable and authored rectangle inputs.",
        "#pragma once",
        "#include <array>",
        "#include <cstdint>",
        "#include <utility>",
        "namespace em2::draw_extent_test_data {",
        "inline constexpr uint32_t shader[] = {" + ", ".join(f"0x{word:08X}u" for word in shader) + "};",
        "struct Clear { std::array<std::pair<uint32_t,uint32_t>,32> registers; std::array<uint8_t,84> vertices; uint32_t max_y; };",
        "inline constexpr Clear clears[] = {",
    ]
    lines.extend(clear_input(width, height) for width, height in dimensions)
    lines.extend(["};", "}", ""])
    text = "\n".join(lines)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if not args.output.exists() or args.output.read_text(encoding="utf-8") != text:
        args.output.write_text(text, encoding="utf-8")
    print(f"Generated {len(dimensions)} authored clear rectangles with the locally extracted shader")


if __name__ == "__main__":
    main()
