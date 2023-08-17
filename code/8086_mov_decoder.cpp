#include <Windows.h>
#include <stdint.h>
#include <string>
typedef int8_t int8;
typedef int16_t int16;
typedef int32_t int32;
typedef uint8_t uint8;
typedef uint16_t uint16;
typedef uint32_t uint32;
typedef uint64_t uint64;
typedef int32 bool32;

#define internal static
#define local_persist static 
#define global_variable static

#if !defined(ASH_SLOW)
#define ASH_SLOW 1
#endif

#define Uint32Max 0xFFFFFFFF

#if ASH_SLOW
#define Assert(Expression) if(!(Expression)) {*(int*)0 = 0;}
#else
#define Assert(Expression)
#endif
#define InvalidCodePath Assert(!"InvalidCodePath")
#define InvalidDefaultCase default: {InvalidCodePath;} break
#define InvalidCase(Value) case Value: {InvalidCodePath;} break

inline uint32
SafeTruncateUInt64(uint64 Value) {
	Assert(Value <= Uint32Max);
	uint32 Result = (uint32)Value;
	return(Result);
}

#pragma region File I/O
internal void
FreeFileMemory(void* Memory) {
	if (Memory) {
		VirtualFree(Memory, 0, MEM_RELEASE);
	}
}

struct debug_read_file_result
{
	uint32 ContentsSize;
	void* Contents;
};

internal debug_read_file_result
ReadEntireFile(char* Filename) {
	debug_read_file_result Result = {};

	HANDLE FileHandle = CreateFileA(Filename, GENERIC_READ, FILE_SHARE_READ, 0, OPEN_EXISTING, 0, 0);
	if (FileHandle != INVALID_HANDLE_VALUE) {
		LARGE_INTEGER FileSize;
		if (GetFileSizeEx(FileHandle, &FileSize)) {
			uint32 FileSize32 = SafeTruncateUInt64(FileSize.QuadPart);
			Result.Contents = VirtualAlloc(0, FileSize32, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
			if (Result.Contents) {
				DWORD BytesRead;
				if (ReadFile(FileHandle, Result.Contents, FileSize32, &BytesRead, 0) && (FileSize32 == BytesRead)) {
					//NOTE(aske): File read successfully
					Result.ContentsSize = FileSize32;
				}
				else {
					FreeFileMemory(Result.Contents);
					Result.Contents = 0;
				}
			}
		}
		CloseHandle(FileHandle);
	}
	return Result;
}
internal bool32
WriteEntireFile(char* Filename, uint32 MemorySize, void* Memory) {
	bool32 Result = false;

	HANDLE FileHandle = CreateFileA(Filename, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0);
	if (FileHandle != INVALID_HANDLE_VALUE) {
		DWORD BytesWritten;
		if (WriteFile(FileHandle, Memory, MemorySize, &BytesWritten, 0)) {
			//NOTE (aske): File read successfully
			Result = (BytesWritten == MemorySize);
		}
		CloseHandle(FileHandle);
	}

	return Result;
}
#pragma endregion

enum DisplacementMode : uint8 { //*ptr + displacementAmount
	Memory_None = 0,
	Memory_8bit = 1,
	Memory_16bit = 2,
	Register_None = 3,
};
enum Register8 : uint8 {
	AccumulatorLow, //Byte Multiply, Byte Divide, Byte I/O, Translate, Decimal Arithmetic
	CountLow, //Variable Shift and Rotate
	DataLow,
	BaseLow,
	AccumulatorHigh, //Byte Multiply, Byte Divide
	CountHigh,
	DataHigh,
	BaseHigh
};
enum Register16 : uint8 {
	AccumulatorXFull, //Word Multiply, Word Divide, Word I/O
	CountXFull, //String Operations, Loops
	DataXFull, //World Multiply, Word Divide, Indirect I/O
	BaseXFull, //Translate
	StackPointer, //Stack Operations
	BasePointer, 
	SourceIndex, //String Operations
	DestinationIndex, //String Operations
};

struct RegisterEncoding {
	union {
		uint8 value;
		Register8 byteRegister;
		Register16 wordRegister;
	};
};


internal std::string
NarrowRegisterEncodingToString(RegisterEncoding encoding) {
	switch (encoding.byteRegister)
	{
		case AccumulatorLow: return "al";
		case CountLow: return "cl";
		case DataLow: return "dl";
		case BaseLow: return "bl";
		case AccumulatorHigh: return "ah";
		case CountHigh: return "ch";
		case DataHigh: return "dh";
		case BaseHigh: return "bh";
		InvalidDefaultCase;
	}
	return "ERR";
}

internal std::string
WideRegisterEncodingToString(RegisterEncoding encoding) {
	switch (encoding.wordRegister)
	{
		case AccumulatorXFull: return "ax";
		case CountXFull: return "cx";
		case DataXFull: return "dx";
		case BaseXFull: return "bx";
		case StackPointer: return "sp";
		case BasePointer: return "bp";
		case SourceIndex: return "si";
		case DestinationIndex: return "di";
		InvalidDefaultCase;
	}
	return "ERR";
}

internal void
Disassemble8086(char* binaryFileName, char* outputFileName) {
	debug_read_file_result file = ReadEntireFile(binaryFileName);
	void* entireBinary = file.Contents;
	uint32 binaryLengthInBytes = file.ContentsSize;
	uint32 outputLength = 9; //"bits 16\n\n"
	std::string output = "bits 16\n";
	uint8* byte1Cursor = (uint8*)entireBinary;
	uint8* byte2Cursor = byte1Cursor + 1;

	for (size_t i = 0; i < binaryLengthInBytes; i+=2)
	{
		if ((*byte1Cursor & 0xFC) == 0x88) //MOV1: Register/memory to/from register
		{
			output += "\nmov ";
			outputLength += 5;
			bool is16Bit = (*byte1Cursor & 0x1); //!8bit
			bool isREGDestination = (*byte1Cursor & 0x2); //REG either specifies Source or Destination

			RegisterEncoding registryEncoding = { (uint8)((*byte2Cursor & 0x38) >> 3) };
			DisplacementMode displacementMode = (DisplacementMode)((uint8)((*byte2Cursor & 0xC0) >> 6));
			displacementMode = displacementMode == Memory_None && registryEncoding.value == 6 ? Memory_16bit : displacementMode;

			switch (displacementMode)
			{
				case Memory_None: Assert("Not implemented"); break;
				case Memory_8bit: Assert("Not implemented"); break;
				case Memory_16bit: Assert("Not implemented"); break;
				case Register_None:
				{
					RegisterEncoding secondRegistryEncoding = { (uint8)((*byte2Cursor & 0x7)) };
					std::string firstOperand;
					std::string secondOperand;

					if (is16Bit)
					{
						firstOperand = WideRegisterEncodingToString(registryEncoding);
						secondOperand = WideRegisterEncodingToString(secondRegistryEncoding);
					}
					else {
						firstOperand = NarrowRegisterEncodingToString(registryEncoding);
						secondOperand = NarrowRegisterEncodingToString(secondRegistryEncoding);
					}
					output += isREGDestination ? firstOperand + ", " + secondOperand : secondOperand + ", " + firstOperand;
					outputLength += 6;
				} break;
				InvalidDefaultCase;
			}
		}
		else { //different instruction
			Assert("Not implemented"); break;
		}

		byte1Cursor += 2;
		byte2Cursor += 2;
	}
	WriteEntireFile(outputFileName, outputLength, const_cast<void*>(static_cast<const void*>(output.c_str())));
}

int main(int argc, char* argv[]) {
	char* inputFileName = argc > 0 ? argv[1] : "listing_0038_many_register_mov";
	char* outputFileName = argc > 1 ? argv[2] : "test.asm";
	Disassemble8086(inputFileName, outputFileName);
	return 0;
}