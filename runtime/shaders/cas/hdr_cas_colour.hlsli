// Linear Rec.709/scRGB <-> normalized linear BT.2020 for HDR presentation.
// Matrices agree with Microsoft's DirectX ColorSpaceUtility reference.
// Conversion before the nonnegative clamp preserves displayable wide-gamut
// colors whose scRGB representation contains negative components.
float3 Hdr709To2020(float3 c) {
    return mul(float3x3(.627402,.329292,.043306,
                        .069095,.919544,.011360,
                        .016394,.088028,.895578),c);
}
float3 Hdr2020To709(float3 c) {
    return mul(float3x3(1.660496,-.587656,-.072840,
                        -.124547,1.132895,-.008348,
                        -.018154,-.100597,1.118751),c);
}
float3 HdrSrgbDecode(float3 c) {
    return float3(c.r<=.04045?c.r/12.92:pow((c.r+.055)/1.055,2.4),
                  c.g<=.04045?c.g/12.92:pow((c.g+.055)/1.055,2.4),
                  c.b<=.04045?c.b/12.92:pow((c.b+.055)/1.055,2.4));
}
float3 HdrCasRead(float3 c) {
    if(hdr_flags&1)return c;
    if(hdr_flags&4)c=HdrSrgbDecode(saturate(c))*(hdr_white/80.0);
    return saturate(Hdr709To2020(c)*(80.0/hdr_peak));
}
float3 HdrCasWrite(float3 c) {
    if(!(hdr_flags&2))return c;
    return Hdr2020To709(saturate(c))*(hdr_peak/80.0);
}
// FSR 1 requires normalized perceptual input. Gamma 2 encodes the calibrated
// BT.2020 range without discarding its HDR luminance or wide-gamut primaries.
float3 HdrFsrRead(float3 c) {
    return (hdr_flags&1)?c:sqrt(HdrCasRead(c));
}
float3 HdrFsrWrite(float3 c) {
    return (hdr_flags&2)?HdrCasWrite(saturate(c)*saturate(c)):c;
}
