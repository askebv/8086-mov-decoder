#include <Windows.h>
#include <stdint.h>
#include <stdio.h>

#include "8086_decoder.h"
#include "string.cpp"
#include "array.cpp"

enum repeat_state
{
	State_repeat_none,
	State_repeat_when_zf_set,	//z=1: rep, repe, repz
	State_repeat_when_zf_clear,	//z=0: repne, repnz
};

struct decoder_state
{
	repeat_state RepeatState;
	b32 endedWithNewLine;

	memory_arena *ScratchPad;
	string_buffer *SegmentOverridePrefix;

#if LABEL_FIRST_PASS
	array_s32 *labelAtByte;
#else
	array_label_position *postPassOffsets;
#endif
};

#pragma region File I/O
internal void FreeFileMemory(void* memory) 
{
	if (memory) {
		VirtualFree(memory, 0, MEM_RELEASE);
	}
}

struct debug_read_file_result
{
	u32 ContentsSize;
	void* Contents;
	s64 CurrentIndex;
};

internal debug_read_file_result ReadEntireFile(char* filename)
{
	debug_read_file_result result = {};
	result.CurrentIndex = -1;

	HANDLE fileHandle = CreateFileA(filename, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
	if (fileHandle != INVALID_HANDLE_VALUE) {
		LARGE_INTEGER fileSize;
		if (GetFileSizeEx(fileHandle, &fileSize)) {
			u32 fileSize32 = SafeTruncateUInt64(fileSize.QuadPart);
			result.Contents = VirtualAlloc(0, fileSize32, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
			if (result.Contents) {
				DWORD bytesRead;
				if (ReadFile(fileHandle, result.Contents, fileSize32, &bytesRead, 0) && (fileSize32 == bytesRead)) {
					//NOTE(aske): File read successfully
					result.ContentsSize = fileSize32;
				}
				else {
					FreeFileMemory(result.Contents);
					result.Contents = 0;
				}
			}
		}
		CloseHandle(fileHandle);
	}
	return result;
}

internal b32 WriteEntireFile(char* filename, u32 memorySize, void* memory)
{
	b32 result = false;

	HANDLE fileHandle = CreateFileA(filename, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0);
	if (fileHandle != INVALID_HANDLE_VALUE) {
		DWORD bytesWritten;
		if (WriteFile(fileHandle, memory, memorySize, &bytesWritten, 0)) {
			//NOTE (aske): File read successfully
			result = (bytesWritten == memorySize);
		}
		CloseHandle(fileHandle);
	}

	return result;
}
#pragma endregion

#if !LABEL_FIRST_PASS
//NOTE (Aske): "\n " is used to determine that it's a label
global_variable u32 labelSpaceSize = 20; //label__2147483647:\n "
#endif

#define ASM_OPERATION(name) void name(decoder_state *decoderState, string_buffer *outputLine, debug_read_file_result *binaryInputFile, u8 currentByte)
typedef ASM_OPERATION(asm_operation);

//primary function container
global_variable asm_operation *asmOps[256] = { 0 };

global_variable char *regs16bit[8]  = { "ax", "cx", "dx", "bx", "sp", "bp", "si", "di" };
global_variable char *regs8bit[8] = { "al", "cl", "dl", "bl", "ah", "ch", "dh", "bh" };
global_variable char **regsAll[2] = { regs8bit, regs16bit }; //regsAll[isWide][regByte]
//NOTE (Aske): rnm[6] requires displacement
global_variable char *rnmForNon3Mods[8] = { "bx + si", "bx + di", "bp + si", "bp + di", "si", "di", "bp", "bx" };

#define PushString(arena, stringPtrName, bufferSize)\
PushStructBuffer(arena, string, bufferSize); 		\
stringPtrName->base = (u8 *)(stringPtrName + 1)

#define PushStringBuffer(arena, stringBufferPtrName, bufferSize)\
PushStructBuffer(arena, string_buffer, bufferSize); 			\
stringBufferPtrName->base = (u8 *)(stringBufferPtrName + 1);	\
stringBufferPtrName->capacity = bufferSize


/*----_---x*/
/*----_--x-*/
#define ParseIsWideIsOp1Dest(firstByte)	\
b32 isWide = (firstByte & 0x1);			\
b32 isOp1Dest = ((firstByte >> 1) & 0x1)

/*xx--_----*/
/*--xx_x---*/
/*----_-xxx*/
#define ParseModRegRnm(secondByte)\
u8 mod = (secondByte >> 6) & 0x3; \
u8 reg = (secondByte >> 3) & 0x7; \
u8 rnm = secondByte & 0x7

#define CastU8HiLoToS16(lowByte, highByte) ((s16)highByte << 8) | lowByte

internal void Append(memory_arena *arena, string *toAppend)
{
	void *writeLocation = PushSize(arena, toAppend->count);
	NaiveWiderCopy(toAppend->count, toAppend->data, writeLocation);
}

#define AppendAndPrint(arena, toAppend) Append(arena, (toAppend)); printf((toAppend)->data)

internal void StringBufferWidePrepend(size_t byteCount, void *source, string_buffer *dest)
{
	Assert(byteCount + dest->count <= dest->capacity);
	NaiveWiderCopy(byteCount,
	               source,
	               dest->base + dest->count);
	dest->count += byteCount;
}

internal u8 GetNextByte(debug_read_file_result *file)
{
	if (++file->CurrentIndex >= file->ContentsSize)
	{
		Assert(!"EndOfFile");
		return 0;
	}

	return *((u8 *)file->Contents + file->CurrentIndex);
}

struct byte_of_file
{
	u8 byte;
	b8 isValid;
};

internal byte_of_file GetNextOpsByte(debug_read_file_result *file)
{
	if (++file->CurrentIndex >= file->ContentsSize)
	{
		return { 0, false };
	}
	;
	return { *((u8 *)file->Contents + file->CurrentIndex), true };
}

internal void GetEffectiveAddressString(string_buffer *output,
                                          debug_read_file_result *binaryInputFile, u8 mod, u8 rnm,
                                          string_buffer *segmentPrefix)
{
	Assert(mod != 3);

	if (mod == 0) // No displacement
	{
		if (rnm == 6) 	//110 - DIRECT ADDRESS "+ 16-bit displacement"
		{
			u8 dlo = GetNextByte(binaryInputFile);
			u8 dhi = GetNextByte(binaryInputFile);
			s16 directAddress = CastU8HiLoToS16(dlo, dhi);
			FormatStringBufferFromBase(output, "%s[%hu]", segmentPrefix->data, directAddress);
		}
		else
		{
			FormatStringBufferFromBase(output, "%s[%s]", segmentPrefix->data, rnmForNon3Mods[rnm]);
		}
	}
	else if (mod == 1) //8-bit displacement
	{
		s8 dlo = GetNextByte(binaryInputFile);
		s16 displacement = dlo;
		FormatStringBufferFromBase(output, displacement == 0 ? "%s[%s]" : "%s[%s %+hd]",
		                           segmentPrefix->data, rnmForNon3Mods[rnm], displacement);
	}
	else //16-bit displacement
	{
		u8 dlo = GetNextByte(binaryInputFile);
		u8 dhi = GetNextByte(binaryInputFile);
		s16 displacement = CastU8HiLoToS16(dlo, dhi);
		FormatStringBufferFromBase(output, displacement == 0 ? "%s[%s]" : "%s[%s %+hd]",
		                           segmentPrefix->data, rnmForNon3Mods[rnm], displacement);
	}
	ZeroSize(segmentPrefix->base, segmentPrefix->count); //clear for future instructions
}

#define ParseEffectiveAddressString(strVarName) 										\
string_buffer *strVarName = PushStringBuffer(decoderState->ScratchPad, strVarName, 24); \
GetEffectiveAddressString(strVarName, binaryInputFile, mod, rnm, decoderState->SegmentOverridePrefix)


internal void ToInstructionLine(string_buffer *output, char *instruction,
                                char *operand1, char *operand2, b32 isOp1Dest)
{
	char *strFormat = "%s %s, %s\n";
	if (isOp1Dest)
	{
		FormatStringBufferFromBase(output, strFormat, instruction, operand1, operand2);
	}
	else
	{
		FormatStringBufferFromBase(output, strFormat, instruction, operand2, operand1);
	}
}

internal ASM_OPERATION(OpNotImplemented)
{
	char helperMsgBuffer[32];
	FormatString(ArrayCount(helperMsgBuffer), helperMsgBuffer, "op[%#4.2hhx] not implemented\n", currentByte);
	printf(helperMsgBuffer);
}

internal ASM_OPERATION(OpNotUsed)
{
	char helperMsgBuffer[32];
	FormatString(ArrayCount(helperMsgBuffer), helperMsgBuffer, "op[%#4.2hhx] not used\n", currentByte);
	printf(helperMsgBuffer);
}

//-------------------------------------------------------------------------
//NOTE (Aske): Below are operations in the order of first appearance
//in table 4-12 of the 8086 1979 user's manual (p. 164).
//Perhaps it would be better to just sort them alphabetically,
//and rely on the 4-13 table for navigating the order?
//-------------------------------------------------------------------------

internal ASM_OPERATION(MovRom)
{
	ParseIsWideIsOp1Dest(currentByte);
	currentByte = GetNextByte(binaryInputFile);
	ParseModRegRnm(currentByte);

	if (mod == 3)
	{
		ToInstructionLine(outputLine, "mov",
		                  regsAll[isWide][reg],
		                  regsAll[isWide][rnm],
		                  isOp1Dest);
	}
	else 
	{
		ParseEffectiveAddressString(strEffectiveAddress);

		ToInstructionLine(outputLine, "mov",
		                  regsAll[isWide][reg],
		                  strEffectiveAddress->data,
		                  isOp1Dest);
	}
}

internal ASM_OPERATION(MovImmToMemory)
{
	b32 isWide = currentByte & 0x1;
	currentByte = GetNextByte(binaryInputFile);
	ParseModRegRnm(currentByte);
	Assert(reg == 0);

	ParseEffectiveAddressString(strEffectiveAddress);

	if (isWide)
	{
		u8 lo = GetNextByte(binaryInputFile);
		u8 hi = GetNextByte(binaryInputFile);
		s16 data = CastU8HiLoToS16(lo, hi);

		FormatStringBufferFromBase(outputLine, "mov %s, word %hd\n", strEffectiveAddress->data, data);
	}
	else
	{
		u8 lo = GetNextByte(binaryInputFile);
		s16 data = lo;

		FormatStringBufferFromBase(outputLine, "mov %s, byte %hd\n", strEffectiveAddress->data, data);
	}
}

internal ASM_OPERATION(MovImmToRegister)
{
	u8 reg = currentByte & 0x7;
	b32 isWide = (currentByte >> 3) & 0x1;
	s8 lo, hi;
	s16 data = lo = GetNextByte(binaryInputFile);
	if (isWide)
	{
		hi = GetNextByte(binaryInputFile);
		data = ((hi & 0xff) << 8) | (lo & 0xff); //NOTE (Aske): &0xff to prevent sign extensions
	}

	FormatStringBufferFromBase(outputLine, "mov %s, %hd\n", regsAll[isWide][reg], data);
}

internal ASM_OPERATION(MovMemAccumulator)
{
	ParseIsWideIsOp1Dest(currentByte);
	u8 lo = GetNextByte(binaryInputFile);
	u8 hi = GetNextByte(binaryInputFile);
	s16 data = CastU8HiLoToS16(lo, hi);

	//TODO (Aske): test if this should respect isWide (al, lo vs. ax, data)
	FormatStringBufferFromBase(outputLine, (isOp1Dest ? "mov [%hd], ax\n" : "mov ax, [%hd]\n"), data);
}

internal ASM_OPERATION(MovSegreg)
{
	b32 isOp1Dest = (currentByte >> 1 ) & 0x1;
	currentByte = GetNextByte(binaryInputFile);
	ParseModRegRnm(currentByte);
	Assert(reg <= 3);
	u8 sr = (currentByte >> 3) & 0x3;

	char *segregs[4] = { "es", "cs", "ss", "ds" };

	if (mod == 3)
	{
		ToInstructionLine(outputLine, "mov", segregs[sr], regs16bit[rnm], isOp1Dest);
	}
	else
	{
		ParseEffectiveAddressString(strEffectiveAddress);
		ToInstructionLine(outputLine, "mov", segregs[sr], strEffectiveAddress->data, isOp1Dest);
	}
}

internal ASM_OPERATION(IncDecCallJmpPushRom16)
{
	currentByte = GetNextByte(binaryInputFile);
	ParseModRegRnm(currentByte);
	Assert(reg != 7);
	//indirect intra-/intersegment-->      intra     inter     intra    inter
	char *instruction[8] = { "inc", "dec", "call", "call far", "jmp", "jmp far", "push", "NOT USED" };
	if (mod == 3)
	{
		FormatStringBufferFromBase(outputLine, "%s word %s\n", instruction[reg], regs16bit[rnm]);
	}
	else
	{
		ParseEffectiveAddressString(strEffectiveAddress);
		FormatStringBufferFromBase(outputLine, "%s word %s\n", instruction[reg], strEffectiveAddress->data);
	}
}

internal ASM_OPERATION(IncDecPushPop)
{
	u8 instIdx = (currentByte >> 3) & 0x3;
	u8 reg = currentByte & 0x7;

	char *instruction[4] = { "inc", "dec", "push", "pop"};
	FormatStringBufferFromBase(outputLine, "%s %s\n", instruction[instIdx], regs16bit[reg]);
}

internal ASM_OPERATION(PushPopSegreg)
{
	b8 isPop = currentByte & 0x1;
	u8 reg = (currentByte >> 3) & 0x3;
	char *segmentRegisters[4] = { "es", "cs", "ss", "ds" };
	FormatStringBufferFromBase(outputLine, (isPop ? "pop %s\n" : "push %s\n"), segmentRegisters[reg]);
}

internal ASM_OPERATION(PopRom16)
{
	currentByte = GetNextByte(binaryInputFile);
	ParseModRegRnm(currentByte);
	Assert(reg == 0);

	if (mod == 3)
	{
		FormatStringBufferFromBase(outputLine, "pop %s\n", regs16bit[rnm]);
	}
	else
	{
		ParseEffectiveAddressString(strEffectiveAddress);
		FormatStringBufferFromBase(outputLine, "pop word %s\n", strEffectiveAddress->data);
	}
}

internal ASM_OPERATION(TestXchgRom)
{
	b32 isWide = currentByte & 0x1;
	u8 index = (currentByte >> 1) & 0x1;
	currentByte = GetNextByte(binaryInputFile);
	ParseModRegRnm(currentByte);
	char *instruction[2] = { "test", "xchg" };

	if (mod == 3)
	{
		ToInstructionLine(outputLine, instruction[index],
		                  regsAll[isWide][reg], regsAll[isWide][rnm], index);
	}
	else
	{
		//NOTE (Aske): xchg operands must be equal size, so "byte"/"word" is unnecessary.
		ParseEffectiveAddressString(strEffectiveAddress);
		FormatStringBufferFromBase(outputLine, "%s %s, %s\n", instruction[index],
		                           strEffectiveAddress->data, regsAll[isWide][reg]);
	}
}

internal ASM_OPERATION(XchgAccReg16)
{
	u8 reg = currentByte & 0x7;
	FormatStringBufferFromBase(outputLine, "xchg ax, %s\n", regs16bit[reg]);
}

internal ASM_OPERATION(InOutFixedPort8)
{
	u8 index = currentByte & 0x3;
	char *instFormat[4] = {
		"in al, %hhu\n",
		"in ax, %hhu\n",
		"out %hhu, al\n",
		"out %hhu, ax\n"
	};
	u8 lo = GetNextByte(binaryInputFile);
	FormatStringBufferFromBase(outputLine, instFormat[index], lo);
}

internal ASM_OPERATION(InOutVariablePort)
{
	u8 index = currentByte & 0x3;
	char *instFormat[4] = {
		"in al, dx\n",
		"in ax, dx\n",
		"out dx, al\n",
		"out dx, ax\n"
	};
	FormatStringBufferFromBase(outputLine, instFormat[index]);
}

internal ASM_OPERATION(Xlat)
{
	FormatStringBufferFromBase(outputLine, "xlat\n");
}

internal ASM_OPERATION(LeaLesLdsRom16)
{
	//1000 1101 Lea -> 11 (1xx1)
	//1100 0100 Les -> 00 (0xx0)
	//1100 0101 Lds -> 01 (0xx1)
	u8 index = ((currentByte & 0x8) >> 2) | (currentByte & 0x1);
	currentByte = GetNextByte(binaryInputFile);
	ParseModRegRnm(currentByte);
	ParseEffectiveAddressString(strEffectiveAddress);
	char *instruction[4] = { "les", "lds", "NOT USED", "lea" };
	FormatStringBufferFromBase(outputLine, "%s %s, %s\n",
	                           instruction[index], regs16bit[reg], strEffectiveAddress->data);
}

internal ASM_OPERATION(CbwCwdWaitPushfPopfSahfLahf)
{
	u8 index = currentByte & 0x7;
	Assert(index != 2);
	char *instruction[8] = { "cbw", "cwd", "NOT USED", "wait", "pushf", "popf", "sahf", "lahf" };
	FormatStringBufferFromBase(outputLine, "%s\n", instruction[index]);
}

internal ASM_OPERATION(AddOrAdcSbbAndSubXorCmpRom)
{
	ParseIsWideIsOp1Dest(currentByte);
	u8 index = (currentByte >> 3) & 0x7;
	char *instruction[8] = { "add", "or", "adc", "sbb", "and", "sub", "xor", "cmp" };

	currentByte = GetNextByte(binaryInputFile);
	ParseModRegRnm(currentByte);
	if (mod == 3)
	{
		ToInstructionLine(outputLine, instruction[index],
		                  regsAll[isWide][reg], regsAll[isWide][rnm],
		                  isOp1Dest);
	}
	else
	{
		ParseEffectiveAddressString(strEffectiveAddress);
		ToInstructionLine(outputLine, instruction[index],
		                  regsAll[isWide][reg], strEffectiveAddress->data,
		                  isOp1Dest);
	}
}

internal ASM_OPERATION(AddOrAdcSbbAndSubXorCmpImmediate)
{
	//sw = 00; 8-bit, no sign extension
	//sw = 01; 16-bit
	//sw = 10; 8-bit, no sign extension
	//sw = 11; 8-bit, sign-extended to 16-bit
	//NOTE (Aske): Since we're only decoding to strings, we only care about bit size
	b32 isWide = currentByte & 0x1;
	b32 is16bit = (currentByte & 0x3) == 1; //s=0, w=1
	b32 isSignExtended = (currentByte >> 1) & 0x1;
	currentByte = GetNextByte(binaryInputFile);
	ParseModRegRnm(currentByte); //NOTE (Aske): reg is the instruction index here
	Assert((!isSignExtended) || (reg != 1 && reg != 4 & reg != 6)); //or, and, xor cannot be sign-extended
	char *instruction[8] = { "add", "or", "adc", "sbb", "and", "sub", "xor", "cmp" };

	char *destination;
	string_buffer *specifiedSize = PushStringBuffer(decoderState->ScratchPad, specifiedSize, 6);
	if (mod == 3)
	{
		destination = regsAll[isWide][rnm];
		Assert(!*specifiedSize->base);
	}
	else
	{
		ParseEffectiveAddressString(strEffectiveAddress);
		destination = strEffectiveAddress->data;
		FormatStringBufferFromBase(specifiedSize, (isWide ? "word " : "byte "));
	}

	s16 data;
	if (is16bit)
	{
		u8 lo = GetNextByte(binaryInputFile);
		u8 hi = GetNextByte(binaryInputFile);
		data = CastU8HiLoToS16(lo, hi);
	}
	else
	{
		s8 lo = GetNextByte(binaryInputFile);
		data = lo;
	}
	FormatStringBufferFromBase(outputLine, "%s %s%s, %hd\n",
	                           instruction[reg], specifiedSize->data, destination, data);
}

internal ASM_OPERATION(AddOrAdcSbbAndSubXorCmpAccumulator)
{
	b32 isWide = (currentByte & 0x1);
	u8 index = (currentByte >> 3) & 0x7;
	char *instruction[8] = { "add", "or", "adc", "sbb", "and", "sub", "xor", "cmp" };
	s8 lo, hi;
	s16 data = lo = GetNextByte(binaryInputFile);
	if (isWide)
	{
		hi = GetNextByte(binaryInputFile);
		data = ((hi & 0xff) << 8) | (lo & 0xff); //NOTE (Aske): &0xff to prevent sign extension
	}

	FormatStringBufferFromBase(outputLine, "%s %s, %hd\n", instruction[index],
	                           (isWide ? "ax" : "al"), data);
}

internal ASM_OPERATION(IncDecRom8)
{
	currentByte = GetNextByte(binaryInputFile);
	ParseModRegRnm(currentByte);

	Assert(reg <= 1); //2-7 not used
	char *instruction[8] = { "inc", "dec", "NOT USED", "NOT USED", "NOT USED", "NOT USED", "NOT USED", "NOT USED" };
	
	if (mod == 3)
	{
		FormatStringBufferFromBase(outputLine, "%s %s\n", instruction[reg], regs8bit[rnm]);
	}
	else
	{
		ParseEffectiveAddressString(strEffectiveAddress);
		FormatStringBufferFromBase(outputLine, "%s byte %s\n", instruction[reg], strEffectiveAddress->data);
	}
}

internal ASM_OPERATION(DaaDasAaaAas)
{
	u8 index = (currentByte >> 3) & 0x3;
	char *instruction[4] = { "daa", "das", "aaa", "aas" };
	FormatStringBufferFromBase(outputLine, "%s\n", instruction[index]);
}

internal ASM_OPERATION(TestNotNegMulImulDivIdivRomImmediate)
{
	b32 isWide = currentByte & 0x1;
	currentByte = GetNextByte(binaryInputFile);
	ParseModRegRnm(currentByte); //NOTE (Aske): reg is the instruction index here
	Assert(reg != 1);
	char *instruction[8] = { "test", "NOT USED", "not", "neg", "mul", "imul", "div", "idiv" };
	
	if (!reg) //Test immediate
	{
		char *destination;
		string_buffer *specifiedSize = PushStringBuffer(decoderState->ScratchPad, specifiedSize, 6);
		if (mod == 3)
		{
			destination = regsAll[isWide][rnm];
			Assert(!*specifiedSize->base);
		}
		else
		{
			ParseEffectiveAddressString(strEffectiveAddress);
			destination = strEffectiveAddress->data;
			FormatStringBufferFromBase(specifiedSize, (isWide ? "word " : "byte "));
		}

		s16 data;
		if (isWide)
		{
			u8 lo = GetNextByte(binaryInputFile);
			u8 hi = GetNextByte(binaryInputFile);
			data = CastU8HiLoToS16(lo, hi);
		}
		else
		{
			s8 lo = GetNextByte(binaryInputFile);
			data = lo;
		}
		FormatStringBufferFromBase(outputLine, "test %s%s, %hd\n",
		                           specifiedSize->data, destination, data);
	}
	else //All other Reg/Mem ops
	{
		if (mod == 3)
		{
			FormatStringBufferFromBase(outputLine, "%s %s\n", 
			                           instruction[reg], regsAll[isWide][rnm]);
		}
		else
		{
			ParseEffectiveAddressString(strEffectiveAddress);
			FormatStringBufferFromBase(outputLine, "%s %s %s\n", instruction[reg], 
			                           (isWide ? "word" : "byte"), strEffectiveAddress->data);
		}
	}
}

internal ASM_OPERATION(AamAad)
{
	b32 isForDivide = currentByte & 0x1;
	currentByte = GetNextByte(binaryInputFile);
	Assert(currentByte == 0xa);
	FormatStringBufferFromBase(outputLine, "%s\n", isForDivide ? "aad" : "aam");
}

internal ASM_OPERATION(RolRorRclRcrSalShlShrSar)
{
	b32 isWide = currentByte & 0x1;
	b32 isCountClSpecified = (currentByte >> 1) & 0x1;
	currentByte = GetNextByte(binaryInputFile);
	ParseModRegRnm(currentByte); //NOTE (Aske): reg is the instruction index here
	Assert(reg != 6);

	//NOTE (Aske): sal and shl are synonymous for left shift.
	//sal is arithmetic and shl is logical.
	//The former could imply a signed number, and unsigned for the latter.
	//But since both left shifts ignore the sign bit, I defaulted to logical.
	char *instruction[8] = { "rol", "ror", "rcl", "rcr", "shl", "shr", "NOT USED", "sar" };

	if (mod == 3)
	{
		FormatStringBufferFromBase(outputLine, "%s %s, %s\n",
		                           instruction[reg], regsAll[isWide][rnm],
		                           (isCountClSpecified ? "cl" : "1"));
	}
	else
	{
		ParseEffectiveAddressString(strEffectiveAddress);
		FormatStringBufferFromBase(outputLine, "%s %s %s, %s\n", instruction[reg], 
		                           (isWide ? "word" : "byte"), strEffectiveAddress->data,
		                           (isCountClSpecified ? "cl" : "1"));
	}
}

internal ASM_OPERATION(TestAccumulator)
{
	b32 isWide = currentByte & 0x1;

	s16 data;
	if (isWide)
	{
		u8 lo = GetNextByte(binaryInputFile);
		u8 hi = GetNextByte(binaryInputFile);
		data = CastU8HiLoToS16(lo, hi);
	}
	else
	{
		s8 lo = GetNextByte(binaryInputFile);
		data = lo;
	}

	FormatStringBufferFromBase(outputLine, "test %s, %hd\n",
	                           (isWide ? "ax" : "al"), data);
}

internal ASM_OPERATION(RepneRepHltCmc)
{
	//NOTE (Aske): rep/repe/repz vs. repne/repnz are synonyms
	//that semantically depend on the succeeding instruction.
	//I'm not well-versed with these semantics but I assume the most sensible is:
	//name: z=1  / z=0
	//movs: rep	 / undefined
	//cmps: repe / repne
	//scas: repz / repnz
	//lods: rep  / undefined
	//stos: rep  / undefined
	u8 index = currentByte & 0x7;
	Assert(index >= 2 && index <= 5);

	char *instruction[8] = { "NOT USED", "NOT USED", "repn", "rep", "hlt\n", "cmc\n", "NOT USED", "NOT USED" };
	if (index <= 3)
	{
		b32 whenZeroFlagIsSet = currentByte & 0x1;
		decoderState->RepeatState = whenZeroFlagIsSet ?
			State_repeat_when_zf_set : State_repeat_when_zf_clear;
		decoderState->endedWithNewLine = false;
	}

	FormatStringBufferFromBase(outputLine, instruction[index]);
	//TODO (Aske): Should this be merged with MovsCmpsStosLodsScas?
	//Removing state and also assuming that it's not possible/plausible
	//to compile rep without a following string instruction
}

internal ASM_OPERATION(MovsCmpsStosLodsScas)
{
	Assert(decoderState->RepeatState != State_repeat_none);
	//TODO (Aske): Assert that no instruction were attempted in between the repeat and this

	u8 index = currentByte & 0xf;
	Assert(index >= 4 && index != 8 && index != 9);

	char *instruction[16] =
	{
		"NOT USED", "NOT USED", "NOT USED", "NOT USED",
		"movsb", 	"movsw", 	"cmpsb", 	"cmpsw",
		"NOT USED", "NOT USED", "stosb", 	"stosw",
		"lodsb", 	"lodsw", 	"scasb", 	"scasw"
	};

	string_buffer *repPrefix = PushStringBuffer(decoderState->ScratchPad, repPrefix, 2);
	if (index == 6 || index == 7) //cmps
	{
		FormatStringBufferFromBase(repPrefix, "e");
	}
	else if (index == 14 || index == 15)  //scas
	{
		FormatStringBufferFromBase(repPrefix, "z");
	}
	else if (decoderState->RepeatState == State_repeat_when_zf_clear)
	{
		//NOTE (Aske): REP + z=0 doesn't make sense for unconditional instructions
		Assert(!"Undefined behavior!");
		FormatStringBufferFromBase(repPrefix, "e");
	}
	else
	{
		Assert(!*repPrefix->base);
	}

	FormatStringBufferFromBase(outputLine, "%s %s\n", repPrefix->data, instruction[index]);
	decoderState->RepeatState = State_repeat_none;
}

internal ASM_OPERATION(CallJumpDirectIntrasegment16)
{
	u8 index = currentByte & 0x1;
	u8 lo = GetNextByte(binaryInputFile);
	u8 hi = GetNextByte(binaryInputFile);
	s16 ipInc = CastU8HiLoToS16(lo, hi);
	//NOTE (Aske): jmp NEAR-LABEL needs to be farther than -128 to +127 bytes from instruction
	Assert(!index || ipInc >= 128  || ipInc < -128);
	char *instruction[2] = { "call", "jmp" };

	//TODO (Aske): Verify it's not +2 (and thus relative to the start of this instruction)
	u64 absoluteAddress = binaryInputFile->CurrentIndex + 1 + ipInc;
	Assert((binaryInputFile->CurrentIndex + 1 + ipInc) <= Sint16Max);
	Assert(absoluteAddress >= 0);
#if LABEL_PADDING
	
	//TODO (Aske): Should we write the label in the OutputPool if it's backwardOffset (ipInc < 0)?
	//It would potentially reduce the lookup time in backwardOffsets
	//But it may require duplicate logic for writing, and requires access to the OutputPool
	if (absoluteAddress < binaryInputFile->ContentsSize)
	{
		ArrayLabelPosAdd(decoderState->postPassOffsets, absoluteAddress, 0);
		FormatStringBufferFromBase(outputLine, "%s label__%d\n", instruction[index], absoluteAddress);
	}
	else
	{
		FormatStringBufferFromBase(outputLine, "%s %d\n", instruction[index], absoluteAddress);
	}
#elif LABEL_FIRST_PASS
	//TODO (Aske): Replace absoluteAddress with label lookup from a first pass
#else
	FormatStringBufferFromBase(outputLine, "%s %u\n", instruction[index], absoluteAddress);
	printf("near-label at %llu\n", absoluteAddress);
#endif
}

internal ASM_OPERATION(CallJumpDirectIntersegment)
{
	u8 index = (currentByte >> 4) & 0x1;
	char *instruction[2] = { "jmp", "call" };

	u8 olo = GetNextByte(binaryInputFile);
	u8 ohi = GetNextByte(binaryInputFile);
	u16 offset = ((u16)ohi << 8) | olo;

	u8 slo = GetNextByte(binaryInputFile);
	u8 shi = GetNextByte(binaryInputFile);
	u16 segment = ((u16)shi << 8) | slo;

	FormatStringBufferFromBase(outputLine, "%s %hd:%hu\n", instruction[index], segment, offset);
}

internal ASM_OPERATION(JumpDirectIntrasegment8)
{
	//NOTE (Aske): JMP SHORT-LABEL (0xeb)
	s8 ipInc = GetNextByte(binaryInputFile);

	s64 absoluteAddress = binaryInputFile->CurrentIndex + 1 + ipInc;
	Assert(absoluteAddress >= 0);
	Assert(absoluteAddress <= Sint32Max); //TODO (Aske): Plausibly "<= Sint16Max" instead? Untested
#if LABEL_PADDING
	
	if (absoluteAddress < binaryInputFile->ContentsSize)
	{
		ArrayLabelPosAdd(decoderState->postPassOffsets, absoluteAddress, 0);
		FormatStringBufferFromBase(outputLine, "jmp short label__%d\n", absoluteAddress);
	}
	else
	{
		FormatStringBufferFromBase(outputLine, "jmp short %d\n", absoluteAddress);
	}
#elif LABEL_FIRST_PASS
	//TODO (Aske): Replace absoluteAddress with label lookup from a first pass
#else 
	FormatStringBufferFromBase(outputLine, "jmp short %u\n", absoluteAddress);
	printf("short label at %lld\n", absoluteAddress);
#endif
}

internal ASM_OPERATION(RetIntraIntersegment)
{
	b32 isAlone = currentByte & 0x1;
	b32 isIntersegment = (currentByte >> 3) & 0x1;
	char *instruction[2] = { "ret", "retf" };
	if (isAlone)
	{
		FormatStringBufferFromBase(outputLine, "%s\n", instruction[isIntersegment]);
	}
	else //adding immediate to SP
	{
		u8 lo = GetNextByte(binaryInputFile);
		u8 hi = GetNextByte(binaryInputFile);
		s16 data = CastU8HiLoToS16(lo, hi);

		FormatStringBufferFromBase(outputLine, "%s %hd\n", instruction[isIntersegment], data);
	}
}

internal ASM_OPERATION(AllJumps)
{
	u8 index = currentByte & 0xf;

	char *instruction[16] =
	{ /*           jnae/jc jae/jnc jz    jnz    jna   jnbe */
		"jo", "jno", "jb", "jnb", "je", "jne", "jbe", "ja",
		"js", "jns", "jp", "jnp", "jl", "jnl", "jle", "jg"
	};/*              jpe   jpo   jnge   jge    jng   jnle */

	s8 relativeAddress = GetNextByte(binaryInputFile);

#if LABEL_PADDING
	s64 absoluteAddress = binaryInputFile->CurrentIndex + 1 + relativeAddress;
	Assert(absoluteAddress >= 0);

	if (absoluteAddress < binaryInputFile->ContentsSize)
	{
		ArrayLabelPosAdd(decoderState->postPassOffsets, absoluteAddress, 0);
		FormatStringBufferFromBase(outputLine, "%s label__%d\n", instruction[index], absoluteAddress);
	}
	else
	{
		FormatStringBufferFromBase(outputLine, "%s $+2%+d\n", instruction[index], relativeAddress);
	}
#elif LABEL_FIRST_PASS
	//TODO (Aske): Replace absoluteAddress with label lookup from a first pass
#else //$ operator is NASM functionality
	FormatStringBufferFromBase(outputLine, "%s $+2%+d\n", instruction[index], relativeAddress);
	printf("relative label at %lld\n", binaryInputFile->CurrentIndex - relativeAddress);
#endif
}

internal ASM_OPERATION(LoopLoopeLoopneJcxz)
{
	u8 index = currentByte & 0x3;

	/*                        loopnz    loopz */
	char *instruction[4] = { "loopne", "loope", "loop", "jcxz" };

	s8 lo = GetNextByte(binaryInputFile);
	s16 relativeAddress = lo;

#if LABEL_PADDING
	s64 absoluteAddress = binaryInputFile->CurrentIndex + 1 + relativeAddress;
	Assert(absoluteAddress >= 0);

	if (absoluteAddress < binaryInputFile->ContentsSize)
	{
		ArrayLabelPosAdd(decoderState->postPassOffsets, absoluteAddress, 0);
		FormatStringBufferFromBase(outputLine, "%s label__%u\n", instruction[index], absoluteAddress);
	}
	else
	{
		FormatStringBufferFromBase(outputLine, "%s $+2%+d\n", instruction[index], relativeAddress);
	}
#elif LABEL_FIRST_PASS
	//TODO (Aske): Replace absoluteAddress with label lookup from a first pass
#else //$ operator is NASM functionality
	FormatStringBufferFromBase(outputLine, "%s $+2%+d\n", instruction[index], relativeAddress);
	printf("relative label at %lld\n", binaryInputFile->CurrentIndex - relativeAddress);
#endif
}

internal ASM_OPERATION(IntIntoIret)
{
	u8 index = currentByte & 0x3;

	char *instruction[4] = { "int3", "int", "into", "iret" };
	
	if (index == 1)
	{
		u8 interruptType = GetNextByte(binaryInputFile);
		FormatStringBufferFromBase(outputLine, "int %hhu\n", interruptType);
	}
	else
	{
		FormatStringBufferFromBase(outputLine, "%s\n", instruction[index]);
	}
}

internal ASM_OPERATION(ClcStcCliStiCldStd)
{
	u8 index = currentByte & 0x7;
	Assert(index <= 5);
	char *instruction[8] = { "clc", "stc", "cli", "sti", "cld", "std", "NOT USED", "NOT USED" };

	FormatStringBufferFromBase(outputLine, "%s\n", instruction[index]);
}

//NOTE (Aske): Not supported by NASM, as far as I can tell.
//TODO (Aske): Try MASM or FASM and see if they can compile it
internal ASM_OPERATION(Escape)
{
	u8 externalOpcode = currentByte & 0x7;
	currentByte = GetNextByte(binaryInputFile);
	ParseModRegRnm(currentByte); //NOTE (Aske): reg = source

	//NOTE (Aske): I'm not sure the "source" is correct. I've been unable to verify it.
	if (mod == 3)
	{
		FormatStringBufferFromBase(outputLine, "esc %hhu %s\n", externalOpcode, regs8bit[reg]);
	}
	else
	{
		ParseEffectiveAddressString(strEffectiveAddress);
		FormatStringBufferFromBase(outputLine, "esc %hhu %s\n", externalOpcode, strEffectiveAddress->data);
	}
}

internal ASM_OPERATION(LockPrefix)
{
	//TODO (Aske): Set state to decoder?
	FormatStringBufferFromBase(outputLine, "lock ");
	decoderState->endedWithNewLine = false;
}

internal ASM_OPERATION(SegmentPrefix)
{
	u8 index = (currentByte >> 3) & 0x3;
	char *instruction[4] = { "es:", "cs:", "ss:", "ds:" };
	FormatStringBufferFromBase(decoderState->SegmentOverridePrefix, instruction[index]);
	decoderState->endedWithNewLine = false;
}

//NOTE (Aske): Trying to be parallel with Table 4-13 in 8086 1979 user's manual (p. 169):
internal void InitializeAsmOpsTable()
{
	for (u8 i = 0x00; i <= 0x03; ++i) asmOps[i] = AddOrAdcSbbAndSubXorCmpRom;
	for (u8 i = 0x04; i <= 0x05; ++i) asmOps[i] = AddOrAdcSbbAndSubXorCmpAccumulator;
	for (u8 i = 0x06; i <= 0x07; ++i) asmOps[i] = PushPopSegreg;
	for (u8 i = 0x08; i <= 0x0b; ++i) asmOps[i] = AddOrAdcSbbAndSubXorCmpRom;
	for (u8 i = 0x0c; i <= 0x0d; ++i) asmOps[i] = AddOrAdcSbbAndSubXorCmpAccumulator;
	asmOps[0x0e]                                = PushPopSegreg;
	asmOps[0x0f]                                = OpNotUsed;
	for (u8 i = 0x10; i <= 0x13; ++i) asmOps[i] = AddOrAdcSbbAndSubXorCmpRom;
	for (u8 i = 0x14; i <= 0x15; ++i) asmOps[i] = AddOrAdcSbbAndSubXorCmpAccumulator;
	for (u8 i = 0x16; i <= 0x17; ++i) asmOps[i] = PushPopSegreg;
	for (u8 i = 0x18; i <= 0x1b; ++i) asmOps[i] = AddOrAdcSbbAndSubXorCmpRom;
	for (u8 i = 0x1c; i <= 0x1d; ++i) asmOps[i] = AddOrAdcSbbAndSubXorCmpAccumulator;
	for (u8 i = 0x1e; i <= 0x1f; ++i) asmOps[i] = PushPopSegreg;
	for (u8 i = 0x20; i <= 0x23; ++i) asmOps[i] = AddOrAdcSbbAndSubXorCmpRom;
	for (u8 i = 0x24; i <= 0x25; ++i) asmOps[i] = AddOrAdcSbbAndSubXorCmpAccumulator;
	asmOps[0x26]                                = SegmentPrefix;
	asmOps[0x27]                                = DaaDasAaaAas;
	for (u8 i = 0x28; i <= 0x2b; ++i) asmOps[i] = AddOrAdcSbbAndSubXorCmpRom;
	for (u8 i = 0x2c; i <= 0x2d; ++i) asmOps[i] = AddOrAdcSbbAndSubXorCmpAccumulator;
	asmOps[0x2e]                                = SegmentPrefix;
	asmOps[0x2f]                                = DaaDasAaaAas;
	for (u8 i = 0x30; i <= 0x33; ++i) asmOps[i] = AddOrAdcSbbAndSubXorCmpRom;
	for (u8 i = 0x34; i <= 0x35; ++i) asmOps[i] = AddOrAdcSbbAndSubXorCmpAccumulator;
	asmOps[0x36]                                = SegmentPrefix;
	asmOps[0x37]                                = DaaDasAaaAas;
	for (u8 i = 0x38; i <= 0x3b; ++i) asmOps[i] = AddOrAdcSbbAndSubXorCmpRom;
	for (u8 i = 0x3c; i <= 0x3d; ++i) asmOps[i] = AddOrAdcSbbAndSubXorCmpAccumulator;
	asmOps[0x3e]                                = SegmentPrefix;
	asmOps[0x3f]                                = DaaDasAaaAas;
	for (u8 i = 0x40; i <= 0x5f; ++i) asmOps[i] = IncDecPushPop;
	for (u8 i = 0x60; i <= 0x6f; ++i) asmOps[i] = OpNotUsed;
	for (u8 i = 0x70; i <= 0x7f; ++i) asmOps[i] = AllJumps;
	for (u8 i = 0x80; i <= 0x83; ++i) asmOps[i] = AddOrAdcSbbAndSubXorCmpImmediate;
	for (u8 i = 0x84; i <= 0x87; ++i) asmOps[i] = TestXchgRom;
	for (u8 i = 0x88; i <= 0x8b; ++i) asmOps[i] = MovRom;
	asmOps[0x8c]                                = MovSegreg;
	asmOps[0x8d]                                = LeaLesLdsRom16;
	asmOps[0x8e]                                = MovSegreg;
	asmOps[0x8f]                                = PopRom16;
	for (u8 i = 0x90; i <= 0x97; ++i) asmOps[i] = XchgAccReg16;
	for (u8 i = 0x98; i <= 0x99; ++i) asmOps[i] = CbwCwdWaitPushfPopfSahfLahf;
	asmOps[0x9a]                                = CallJumpDirectIntersegment;
	for (u8 i = 0x9b; i <= 0x9f; ++i) asmOps[i] = CbwCwdWaitPushfPopfSahfLahf;
	for (u8 i = 0xa0; i <= 0xa3; ++i) asmOps[i] = MovMemAccumulator;
	for (u8 i = 0xa4; i <= 0xa7; ++i) asmOps[i] = MovsCmpsStosLodsScas;
	for (u8 i = 0xa8; i <= 0xa9; ++i) asmOps[i] = TestAccumulator;
	for (u8 i = 0xaa; i <= 0xaf; ++i) asmOps[i] = MovsCmpsStosLodsScas;
	for (u8 i = 0xb0; i <= 0xbf; ++i) asmOps[i] = MovImmToRegister;
	for (u8 i = 0xc0; i <= 0xc1; ++i) asmOps[i] = OpNotUsed;
	for (u8 i = 0xc2; i <= 0xc3; ++i) asmOps[i] = RetIntraIntersegment;
	for (u8 i = 0xc4; i <= 0xc5; ++i) asmOps[i] = LeaLesLdsRom16;
	for (u8 i = 0xc6; i <= 0xc7; ++i) asmOps[i] = MovImmToMemory;
	for (u8 i = 0xc8; i <= 0xc9; ++i) asmOps[i] = OpNotUsed;
	for (u8 i = 0xca; i <= 0xcb; ++i) asmOps[i] = RetIntraIntersegment;
	for (u8 i = 0xcc; i <= 0xcf; ++i) asmOps[i] = IntIntoIret;
	for (u8 i = 0xd0; i <= 0xd3; ++i) asmOps[i] = RolRorRclRcrSalShlShrSar;
	for (u8 i = 0xd4; i <= 0xd5; ++i) asmOps[i] = AamAad;
	asmOps[0xd6]                                = OpNotUsed;
	asmOps[0xd7]                                = Xlat;
	for (u8 i = 0xd8; i <= 0xdf; ++i) asmOps[i] = Escape;
	for (u8 i = 0xe0; i <= 0xe3; ++i) asmOps[i] = LoopLoopeLoopneJcxz;
	for (u8 i = 0xe4; i <= 0xe7; ++i) asmOps[i] = InOutFixedPort8;
	for (u8 i = 0xe8; i <= 0xe9; ++i) asmOps[i] = CallJumpDirectIntrasegment16;
	asmOps[0xea]                                = CallJumpDirectIntersegment;
	asmOps[0xeb]                                = JumpDirectIntrasegment8;
	for (u8 i = 0xec; i <= 0xef; ++i) asmOps[i] = InOutVariablePort;
	asmOps[0xf0]                                = LockPrefix;
	asmOps[0xf1]                                = OpNotUsed;
	for (u8 i = 0xf2; i <= 0xf5; ++i) asmOps[i] = RepneRepHltCmc;
	for (u8 i = 0xf6; i <= 0xf7; ++i) asmOps[i] = TestNotNegMulImulDivIdivRomImmediate;
	for (u8 i = 0xf8; i <= 0xfd; ++i) asmOps[i] = ClcStcCliStiCldStd;
	asmOps[0xfe]                                = IncDecRom8;
	asmOps[0xff]                                = IncDecCallJmpPushRom16;

	for (s16 i = 0x00; i <= 0xff; ++i) Assert(asmOps[i]);
}

#if LABEL_FIRST_PASS
#include "label_pass.cpp"
#endif

int main(int argc, char* argv[])
{
	char* binaryFilePath = argc > 1 ? argv[1] : "..\\data\\listing_0042_completionist_decode";
	char* outputAsmFileName = argc > 2 ? argv[2] : "..\\data\\test42.asm";

#ifdef ASH_INTERNAL
	LPVOID baseAddress = (LPVOID)Terabytes(2);
#else
	LPVOID baseAddress = 0;
#endif

	size_t outputMaxSize = Megabytes(16);
	size_t scratchPadSize = Megabytes(17);
	//Auto-zeroed
	void* allocatedMemory = VirtualAlloc(baseAddress,
	                                     outputMaxSize + scratchPadSize,
	                                     MEM_COMMIT | MEM_RESERVE,
	                                     PAGE_READWRITE);

	memory_arena outputPool = {};
	outputPool.base = (u8 *)allocatedMemory;
	Assert(outputPool.base != 0);
	outputPool.size = outputMaxSize;

	memory_arena scratchPad = {};
	scratchPad.base = (u8 *)allocatedMemory + outputMaxSize;
	Assert(scratchPad.base != 0);
	scratchPad.size = scratchPadSize;

	//'x' 's' ':' '\0'
	string_buffer *segmentOverridePrefix = PushStringBuffer(&scratchPad, segmentOverridePrefix, 4);
	
	decoder_state decoderState = {};
	decoderState.ScratchPad = &scratchPad;
	decoderState.SegmentOverridePrefix = segmentOverridePrefix;

	
	debug_read_file_result file = ReadEntireFile(binaryFilePath);
	void* entireBinary = file.Contents;
	u32 binaryLengthInBytes = file.ContentsSize;

	byte_of_file byteCursor;

	/*
	 * NOTE (Aske): I'm not sure how to handle labels.
	 * My ideas were:
	 * 1. while decoding labels, keep a list of all the label locations and all the instruction lines.
	 * 		And when you've parsed all instructions,
	 * 		do a second pass where you build the final string output, including labels, from both lists.
	 * 
	 * 2. to do a label pass first, locating all the label locations.
	 * 		Keeping them in an array, that you may sort before use.
	 * 		Labels are then added together with the instruction pass.
	 * 
	 * 3. before every (full) instruction line, add enough room for a label,
	 * 		and (over)write that label when encountered.
	 * 		You might keep the "label space" locations in an expanding array.
	 * 
	 * I first went with idea 2, but thought it was too complicated so I went with idea 3 instead.
	 * Idea 1 might be even simpler, but it felt like a solution from GC'ed languages that treat
	 * "inserting" strings into other strings without concern for its cost. I wanted to not do that.
	 * But now that I'm done, I'm not sure idea 3 really was less complicated or efficient.
	* */
#if LABEL_FIRST_PASS
	array_s32 *labelDefinitions = PushPolyArray(&scratchPad, labelDefinitions, Megabytes(7), s32);
	decoderState.labelAtByte = labelDefinitions;
	InitializeLabelPassTable();

	while ((byteCursor = GetNextOpsByte(&file)).isValid)
	{
		u8 currentByte = byteCursor.byte;
		//TODO (Aske): Assert rep states (f2-f5) follow a string instruction (a4-a7, aa-af)
		lpOps[currentByte](&decoderState, &file, currentByte);
	}

	file.CurrentIndex = 0; //NOTE (Aske): Reset cursor for second parse

	//NOTE (Aske): Probably not necessary to clean up the scratchpad, but it's easy to do.
	//If this was production code, we'd manage the memory differently anyway
	size_t labelExcessBuffer = labelDefinitions->size - labelDefinitions->count;
	PopSize(&scratchPad, labelExcessBuffer);
	labelDefinitions->size = labelDefinitions->count;
	
	Array32MergeSort(labelDefinitions, &scratchPad);
		
	s64 labelIdx = 0;
	s32 nextLabelByte = Array32Get(decoderState.labelAtByte, labelIdx);
#elif LABEL_PADDING
	//TODO (Aske): These needs a rename once the functionality is done
	array_label_position *labelPosFromInstructionPass = PushPolyArray(&scratchPad,
	                                                  labelPosFromInstructionPass, Megabytes(3), label_position);
	labelPosFromInstructionPass->type = Type_label_backward;
	array_label_position *forwardOffsets = PushPolyArray(&scratchPad, forwardOffsets, Megabytes(3), label_position);
	labelPosFromInstructionPass->type = Type_label_forward;
	decoderState.postPassOffsets = forwardOffsets;
#endif
	InitializeAsmOpsTable();

	
	//NOTE (Aske): 33000 is an arbitrary limit, guided by Windows
	s64 binaryFilePathLength;
	StringLengthBounded(binaryFilePath, 33000, &binaryFilePathLength);
	size_t binaryFileNameOffset;
	//NOTE (Aske): I'm not sure if I needed to handle both slash types. My MVP skills need practicing.
	StringLastIndexOf2(binaryFilePath, binaryFilePathLength, '\\', '/', &binaryFileNameOffset);
	//NOTE (Aske): remove slash from binaryFileNameOffset
	size_t binaryFileNameLength = binaryFilePathLength - (++binaryFileNameOffset);
	
	string_buffer *sourceFileComment = PushStringBuffer(&scratchPad, sourceFileComment,
	                                                    binaryFileNameLength + 3);
	FormatStringBufferFromBase(sourceFileComment, ";%s\n", binaryFilePath + binaryFileNameOffset);
	AppendAndPrint(&outputPool, &sourceFileComment->asString);

	string enforce16BitAsm = { 8, "bits 16\n" };
	decoderState.endedWithNewLine = true;
	AppendAndPrint(&outputPool, &enforce16BitAsm);
	while ((byteCursor = GetNextOpsByte(&file)).isValid)
	{
		u8 b = byteCursor.byte; //currentByte
		SaveArena(&scratchPad);
		
#if LABEL_FIRST_PASS
		if (b == nextLabelByte)
		{
			string_buffer *label = PushStringBuffer(&scratchPad, label, 48);
			FormatStringBufferFromBase(label, "\nlabel__%u:\n", file.CurrentIndex);
			AppendAndPrint(&outputPool, &label->asString);

			nextLabelByte = Array32Get(decoderState.labelAtByte, ++labelIdx);
		}
		else
		{
			while (b > nextLabelByte)
			{
				Assert(!"Skipped a label");
				nextLabelByte = Array32Get(decoderState.labelAtByte, ++labelIdx);
			}
		}
#elif LABEL_PADDING
		if (decoderState.endedWithNewLine)
		{
			label_position labelPos;

			size_t stringPoolPos = outputPool.used;
			//NOTE (Aske): The label is already established from previous forward-looking jumps
			b32 isPrerequested = ArrayLabelPosFromFilebyte(forwardOffsets, file.CurrentIndex, &labelPos);
			if (isPrerequested)
			{
				string *label = PushString(&scratchPad, label, labelSpaceSize);
				label->count = FormatString(labelSpaceSize, label->data,
				                             "label__%u:\n", file.CurrentIndex);
				Assert(label->count == labelSpaceSize);

				Append(&outputPool, label);
			}
			else // Make space for future backward-looking label references
			{
				//NOTE (Aske): We wouldn't need to Make sure there's room for a null-terminator
				//if we didn't use 
				string *labelSpace = PushString(&scratchPad, labelSpace, labelSpaceSize + 1);
				labelSpace->count = FormatString(labelSpaceSize, labelSpace->data,
				                                 "                    ");
				Assert(labelSpace->count == labelSpaceSize);
				
				Append(&outputPool, labelSpace);
			}
			ArrayLabelPosAdd(labelPosFromInstructionPass, file.CurrentIndex, stringPoolPos);
		}
		//NOTE (Aske): Most instructions end with \n
		decoderState.endedWithNewLine = true;
#endif

		string_buffer *asmInstructionLine = PushStringBuffer(&scratchPad, asmInstructionLine, 64);

		asmOps[b](&decoderState, asmInstructionLine, &file, b);

		AppendAndPrint(&outputPool, &asmInstructionLine->asString);
		ZeroRestoreArena(&scratchPad);
	}

#if LABEL_PADDING
	//NOTE (Aske): Insert missing labels
	for (label_position *it = forwardOffsets->base;
		it < forwardOffsets->base + forwardOffsets->count; ++it)
	{
		label_position preWrittenLabelPos;
		if (ArrayLabelPosFromFilebyteSorted(labelPosFromInstructionPass, it->byteAddress, &preWrittenLabelPos))
		{
			string label = { labelSpaceSize, (char *)outputPool.base + preWrittenLabelPos.poolOffset };
			if (label.data[0] == ' ')
			{
				size_t indexNullTerminator = FormatString(labelSpaceSize - 1, label.data,
				                                          "label__%u:\n           ", it->byteAddress);
				Assert(labelSpaceSize - 1 == indexNullTerminator);
				//NOTE (Aske): Overwrite null-terminator
				label.data[indexNullTerminator] = ' ';
			}
		}
		else
		{
			InvalidCodePath;
		}
	}

	//NOTE (Aske): Remove unused label spaces before every instruction
	string_buffer *trimmedOutput = PushStringBuffer(&scratchPad, trimmedOutput, outputPool.used);
	s64 copyFrom = 0;
	for (label_position *it = labelPosFromInstructionPass->base;
		it < labelPosFromInstructionPass->base + labelPosFromInstructionPass->count; ++it)
	{
		//Copy everything up until the first space
		char *pretrimmedCursor = (char *)outputPool.base + it->poolOffset;
		b32 labelExist = (*pretrimmedCursor != ' ');
		if (labelExist)
		{
			s32 labelSize = 0;
			while (*(pretrimmedCursor++) != '\n')
			{
				Assert(*pretrimmedCursor && (labelSize++ <= labelSpaceSize));
			}
			
		}
		Assert(*pretrimmedCursor == ' ');
		StringBufferWidePrepend(pretrimmedCursor - (char *)outputPool.base - copyFrom,
		                        (outputPool.base + copyFrom), trimmedOutput);

		//Skip the spaces
		s32 skipCount = 0;
		while (*(++pretrimmedCursor) == ' ')
		{
			Assert(skipCount++ <= labelSpaceSize);
		}

		copyFrom = pretrimmedCursor - (char *)outputPool.base;
	}

	u8 *remainderStart = outputPool.base + copyFrom;
	u8 *endOfPretrim = outputPool.base + outputPool.used;
	StringBufferWidePrepend(endOfPretrim - remainderStart, remainderStart, trimmedOutput);

	Assert(!(*endOfPretrim));
	WriteEntireFile(outputAsmFileName, trimmedOutput->count, trimmedOutput->base);

#else
	WriteEntireFile(outputAsmFileName, outputPool.used - 1, outputPool.base);
#endif

	printf("Wrote to completion: %s\n", outputAsmFileName);

	return 0;
}