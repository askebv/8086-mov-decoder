
internal void SkipBytes(debug_read_file_result *file, s64 count)
{
    Assert((file->CurrentIndex + count) <= file->ContentsSize);
    file->CurrentIndex += count;
}

#define LABEL_PASS_OPERATION(name) void name(decoder_state *decoderState, debug_read_file_result *binaryInputFile, u8 currentByte)
typedef LABEL_PASS_OPERATION(label_pass_operation);

global_variable label_pass_operation *lpOps[256] = { 0 };



//-------------------------------------------------------------------------
//NOTE (Aske): First pass operations, to determine label locations
//-------------------------------------------------------------------------

internal LABEL_PASS_OPERATION(LabelPassModRnm)
{
    currentByte = GetNextByte(binaryInputFile);
    ParseModRegRnm(currentByte);
    if (mod == 0 && rnm == 6)
    {
        SkipBytes(binaryInputFile, 2);
    }
    else if (mod == 1)
    {
        SkipBytes(binaryInputFile, 1);
    }
    else if (mod == 2)
    {
        SkipBytes(binaryInputFile, 2);
    }
}

internal LABEL_PASS_OPERATION(LabelPassModRnmSignExtension)
{
    b32 is16bit = (currentByte & 0x3) == 1; //s=0, w=1
    LabelPassModRnm(decoderState, binaryInputFile, currentByte);
    SkipBytes(binaryInputFile, is16bit ? 2 : 1);
}

internal LABEL_PASS_OPERATION(LabelPassModRnmWidthLast)
{
    b32 isWide = currentByte & 0x1;
    LabelPassModRnm(decoderState, binaryInputFile, currentByte);
    SkipBytes(binaryInputFile, isWide ? 2 : 1);
}

internal LABEL_PASS_OPERATION(LabelPassWidthLast)
{
    b32 isWide = currentByte & 0x1;
    SkipBytes(binaryInputFile, isWide ? 2 : 1);
}

internal LABEL_PASS_OPERATION(LabelPassWidthFifth)
{
    b32 isWide = (currentByte >> 3) & 0x1;
    SkipBytes(binaryInputFile, isWide ? 2 : 1);
}

internal LABEL_PASS_OPERATION(LabelPassNop)
{
}

internal LABEL_PASS_OPERATION(LabelPassSkipOne)
{
    SkipBytes(binaryInputFile, 1);
}

internal LABEL_PASS_OPERATION(LabelPassSkipTwo)
{
    SkipBytes(binaryInputFile, 2);
}

internal LABEL_PASS_OPERATION(LabelPassSkipThree)
{
    SkipBytes(binaryInputFile, 3);
}

internal LABEL_PASS_OPERATION(LabelPassSkipFour)
{
    SkipBytes(binaryInputFile, 4);
}

internal LABEL_PASS_OPERATION(LabelPassAloneOrWide)
{
    b32 isWide = currentByte & 0x1;
    if (isWide) SkipBytes(binaryInputFile, 2);
}

internal LABEL_PASS_OPERATION(LabelPassGroup1)
{
    b32 isWide = currentByte & 0x1;
    currentByte = GetNextByte(binaryInputFile);
    ParseModRegRnm(currentByte);
    if (mod == 0 && rnm == 6)
    {
        SkipBytes(binaryInputFile, 2);
    }
    else if (mod == 1)
    {
        SkipBytes(binaryInputFile, 1);
    }
    else if (mod == 2)
    {
        SkipBytes(binaryInputFile, 2);
    }

    if (!reg)
    {
        SkipBytes(binaryInputFile, isWide ? 2 : 1);
    }
}

internal LABEL_PASS_OPERATION(LabelPassShort)
{
    s8 relative = GetNextByte(binaryInputFile);
    s32 absolute = binaryInputFile->CurrentIndex + relative + 1;
    Assert(absolute <= Sint16Max);
    //TODO (Aske): This array isn't sorted at all, so fix this assumption
    s64 idx;
    if (absolute >= 0 && absolute < binaryInputFile->ContentsSize
        && !Array32SortedIndexOf(decoderState->labelAtByte, absolute, &idx))
    {
        Array32Add(decoderState->labelAtByte, absolute);
    }
}

internal LABEL_PASS_OPERATION(LabelPassNear)
{
    u8 lo = GetNextByte(binaryInputFile);
    u8 hi = GetNextByte(binaryInputFile);
    s16 relative = CastU8HiLoToS16(lo, hi);

    s32 absolute = binaryInputFile->CurrentIndex + relative + 1;
    Assert(absolute <= Sint16Max);
    s64 idx;
    if (absolute >= 0 && absolute < binaryInputFile->ContentsSize
        && !Array32SortedIndexOf(decoderState->labelAtByte, absolute, &idx))
    {
        Array32Add(decoderState->labelAtByte, absolute);
    }
}

//NOTE (Aske): Trying to be parallel with Table 4-13 in 8086 1979 user's manual (p. 169):
internal void InitializeLabelPassTable()
{
    for (u8 i = 0x00; i <= 0x03; ++i) lpOps[i] = LabelPassModRnm;
    for (u8 i = 0x04; i <= 0x05; ++i) lpOps[i] = LabelPassWidthLast;
    for (u8 i = 0x06; i <= 0x0b; ++i) lpOps[i] = LabelPassNop;
    for (u8 i = 0x0c; i <= 0x0d; ++i) lpOps[i] = LabelPassWidthLast;
    for (u8 i = 0x0e; i <= 0x13; ++i) lpOps[i] = LabelPassNop;
    for (u8 i = 0x14; i <= 0x15; ++i) lpOps[i] = LabelPassWidthLast;
    for (u8 i = 0x16; i <= 0x1b; ++i) lpOps[i] = LabelPassNop;
    for (u8 i = 0x1c; i <= 0x1d; ++i) lpOps[i] = LabelPassWidthLast;
    for (u8 i = 0x1e; i <= 0x23; ++i) lpOps[i] = LabelPassNop;
    for (u8 i = 0x24; i <= 0x25; ++i) lpOps[i] = LabelPassWidthLast;
    for (u8 i = 0x26; i <= 0x2b; ++i) lpOps[i] = LabelPassNop;
    for (u8 i = 0x2c; i <= 0x2d; ++i) lpOps[i] = LabelPassWidthLast;
    for (u8 i = 0x2e; i <= 0x33; ++i) lpOps[i] = LabelPassNop;
    for (u8 i = 0x34; i <= 0x35; ++i) lpOps[i] = LabelPassWidthLast;
    for (u8 i = 0x36; i <= 0x3b; ++i) lpOps[i] = LabelPassNop;
    for (u8 i = 0x3c; i <= 0x3d; ++i) lpOps[i] = LabelPassWidthLast;
    for (u8 i = 0x3e; i <= 0x6f; ++i) lpOps[i] = LabelPassNop;
    for (u8 i = 0x70; i <= 0x7f; ++i) lpOps[i] = LabelPassShort;
    for (u8 i = 0x80; i <= 0x83; ++i) lpOps[i] = LabelPassModRnmSignExtension;
    for (u8 i = 0x84; i <= 0x8f; ++i) lpOps[i] = LabelPassModRnm;
    for (u8 i = 0x90; i <= 0x99; ++i) lpOps[i] = LabelPassNop;
    lpOps[0x9a]                                = LabelPassSkipThree;
    for (u8 i = 0x9b; i <= 0x9f; ++i) lpOps[i] = LabelPassNop;
    for (u8 i = 0xa0; i <= 0xa3; ++i) lpOps[i] = LabelPassSkipTwo;
    for (u8 i = 0xa4; i <= 0xa7; ++i) lpOps[i] = LabelPassNop;
    for (u8 i = 0xa8; i <= 0xa9; ++i) lpOps[i] = LabelPassWidthLast;
    for (u8 i = 0xaa; i <= 0xaf; ++i) lpOps[i] = LabelPassNop;
    for (u8 i = 0xb0; i <= 0xbf; ++i) lpOps[i] = LabelPassWidthFifth;
    for (u8 i = 0xc0; i <= 0xc1; ++i) lpOps[i] = LabelPassNop;
    for (u8 i = 0xc2; i <= 0xc3; ++i) lpOps[i] = LabelPassAloneOrWide;
    for (u8 i = 0xc4; i <= 0xc5; ++i) lpOps[i] = LabelPassModRnm;
    for (u8 i = 0xc6; i <= 0xc7; ++i) lpOps[i] = LabelPassModRnmWidthLast;
    for (u8 i = 0xc8; i <= 0xc9; ++i) lpOps[i] = LabelPassNop;
    for (u8 i = 0xca; i <= 0xcb; ++i) lpOps[i] = LabelPassAloneOrWide;
    lpOps[0xcc]                                = LabelPassNop;
    lpOps[0xcd]                                = LabelPassSkipOne;
    for (u8 i = 0xce; i <= 0xcf; ++i) lpOps[i] = LabelPassNop;
    for (u8 i = 0xd0; i <= 0xd3; ++i) lpOps[i] = LabelPassModRnm;
    for (u8 i = 0xd4; i <= 0xd7; ++i) lpOps[i] = LabelPassNop;
    for (u8 i = 0xd8; i <= 0xdf; ++i) lpOps[i] = LabelPassModRnm;
    for (u8 i = 0xe0; i <= 0xe3; ++i) lpOps[i] = LabelPassShort;
    for (u8 i = 0xe4; i <= 0xe7; ++i) lpOps[i] = LabelPassSkipOne;
    for (u8 i = 0xe8; i <= 0xe9; ++i) lpOps[i] = LabelPassNear;
    lpOps[0xea]                                = LabelPassSkipFour;
    lpOps[0xeb]                                = LabelPassShort;
    for (u8 i = 0xec; i <= 0xf5; ++i) lpOps[i] = LabelPassNop;
    for (u8 i = 0xf6; i <= 0xf7; ++i) lpOps[i] = LabelPassGroup1;
    for (u8 i = 0xf8; i <= 0xfd; ++i) lpOps[i] = LabelPassNop;
    for (u16 i = 0xfe; i <= 0xff; ++i)lpOps[i] = LabelPassModRnm;

    for (s16 i = 0x00; i <= 0xff; ++i) Assert(lpOps[i]);
}