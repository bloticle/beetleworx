"""Exhaust the transfer division domain at both allowed RCP error endpoints.

This is a numerical proof companion, not a replacement for the physical-GPU
DXBC UDiv comparison in bounded_transfer_division_test.cpp.
"""
import argparse
import json
import math
import struct
from fractions import Fraction
from pathlib import Path


def f32(x):
    return struct.unpack("<f", struct.pack("<f", x))[0]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    bias, error, rounding = Fraction(1, 512), Fraction(1, 2**21), Fraction(1, 2**24)
    # Exact rational proof: all n+b are representable, and hardware's worst
    # allowed RCP and float multiply errors leave the quotient between n/d
    # and (n+1)/d. Every interval has the same floor as integer n/d.
    lower = (2047+bias)*(1-error)*(1-rounding)-2047
    upper = (2047+bias)*(1+error)*(1+rounding)-2047
    assert 0 < lower < upper < 1
    checked = 0
    invalid = 0
    for divisor in range(1, 1024):
        # Using double endpoints includes rounding slightly outside the legal
        # float RCP range, making this test conservatively more demanding.
        reciprocals = [f32((1-float(error))/divisor), f32(1/divisor), f32((1+float(error))/divisor)]
        for numerator in range(2048):
            expected_q, expected_r = divmod(numerator, divisor)
            for reciprocal in reciprocals:
                quotient = int(f32(f32(numerator+float(bias))*reciprocal))
                remainder = numerator-quotient*divisor
                invalid += (quotient, remainder) != (expected_q, expected_r)
                checked += 1
    report = {
        "domain": {"numerator": [0, 2047], "positive_pitch": [1, 1023]},
        "rcp_relative_error_bound": "2^-21 (Direct3D Shader Model 5)",
        "float_multiply_relative_rounding_bound": "2^-24",
        "exact_addition": "All n+1/512 require <=20 significand bits, within binary32's 24.",
        "worst_effective_numerator_bias": [float(lower), float(upper)],
        "strict_quotient_bound_proved": "n/d < computed_float_quotient < (n+1)/d for positive d",
        "endpoint_and_nearest_cases": checked,
        "invalid": invalid,
        "zero_divisor": "Explicit DXBC mask returns UINT_MAX for quotient and remainder; verified separately by GPU oracle.",
        "rcp_source": "https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/rcp--sm5---asm-",
        "udiv_source": "https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/udiv--sm4---asm-",
        "scope": "Arithmetic correctness proof and exhaustive CPU emulation of worst RCP endpoints. No gameplay FPS claim.",
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2)+"\n")
    print(json.dumps(report))
    if invalid:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
